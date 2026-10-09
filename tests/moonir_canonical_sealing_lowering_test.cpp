#include "moonir/MoonIR.h"
#include "moonir/ContainerModel.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "codegen/CodeGenerator.h"
#include "codegen/NativeOwnedResultFacts.h"
#include "diagnostics/Diagnostic.h"
#include "runtime/RuntimeDescriptor.h"
#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeFragmentCompilerBridge.h"
#include "runtime/NativeArtifactABI.h"
#include "tooling/AnalysisSnapshot.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Verifier.h>

static unsigned privateRefJitDropProbeCalls = 0;
static unsigned privateRefJitConversionDropProbeCalls = 0;
static unsigned privateRefJitReturnedDropProbeCalls = 0;
static unsigned privateRefJitReturnedOkDropProbeCalls = 0;
static unsigned privateRefJitReturnedPairDropProbeCalls = 0;
static unsigned privateRefJitAllDropProbeCalls = 0;
static std::vector<int32_t> privateRefJitConversionDropOrder;
static std::vector<int32_t> privateRefJitFromResourceDropOrder;
static std::vector<int32_t> privateRefJitFromBranchMarkers;
static std::vector<int32_t> privateRefJitNestedDropOrder;
static bool privateRefJitDropProbeValid = true;

extern "C" void luna_private_ref_drop_probe(int32_t marker) {
    ++privateRefJitAllDropProbeCalls;
    if (marker == 31) ++privateRefJitDropProbeCalls;
    else if (marker == 47) {
        ++privateRefJitConversionDropProbeCalls;
        privateRefJitConversionDropOrder.push_back(marker);
        privateRefJitFromResourceDropOrder.push_back(marker);
    } else if (marker == 43 || marker == -43) {
        privateRefJitConversionDropOrder.push_back(marker);
        privateRefJitFromResourceDropOrder.push_back(marker);
    } else if (marker == 51 || marker == 53) {
        privateRefJitFromBranchMarkers.push_back(marker);
    } else if (marker == 59) {
        ++privateRefJitReturnedDropProbeCalls;
        privateRefJitFromResourceDropOrder.push_back(marker);
    }
    else if (marker == 67) ++privateRefJitReturnedOkDropProbeCalls;
    else if (marker == 66) ++privateRefJitReturnedPairDropProbeCalls;
    else if (marker == 71 || marker == 73 ||
             marker == 79 || marker == 81 || marker == 83 ||
             marker == 89 || marker == 91 || marker == 93)
        privateRefJitNestedDropOrder.push_back(marker);
    else privateRefJitDropProbeValid = false;
}

namespace canonical_test {

namespace {

unsigned privateRefJitExecutions = 0;
unsigned privateRefJitCompletedResumes = 0;
int32_t privateRefJitResumeStatus = -1;
const char* privateRefJitSlotId = nullptr;
const char* privateRefJitSlotContract = nullptr;
const char* privateRefJitLayoutId = nullptr;
uint64_t privateRefJitArgumentsSize = 0;
uint64_t privateRefJitArgumentsAlignment = 1;
bool privateRefJitPayloadValid = true;
std::vector<int32_t> privateRefJitObservedArguments;
std::vector<unsigned> privateRefJitObservedSources;
std::array<const char*, 2> privateRefJitExpectedSlotIds{};
std::array<const char*, 2> privateRefJitExpectedSlotContracts{};
std::array<const char*, 2> privateRefJitExpectedLayoutIds{};
std::array<uint64_t, 2> privateRefJitExpectedSizes{};
std::array<uint64_t, 2> privateRefJitExpectedAlignments{};

// Test-only owner cell: its JIT code lease lives until the frozen Drop thunk
// has consumed the owner. Copies cannot create a second owning carrier.
struct PrivateRefReturnedOwner {
    using DropEntry = int32_t (*)(void**);

    PrivateRefReturnedOwner(void* owned, DropEntry thunk,
                            std::shared_ptr<LunaJitModule> code)
        : owner(owned), drop(thunk), lease(std::move(code)) {}
    PrivateRefReturnedOwner(const PrivateRefReturnedOwner&) = delete;
    PrivateRefReturnedOwner& operator=(const PrivateRefReturnedOwner&) = delete;
    ~PrivateRefReturnedOwner() {
        if (owner && lease && drop) drop(&owner);
    }

    bool live() const { return owner && drop && lease; }
    int32_t dropOnce() {
        if (!live()) return 1;
        const int32_t status = drop(&owner);
        if (status == 0 && !owner) {
            drop = nullptr;
            lease.reset();
        }
        return status;
    }

private:
    void* owner = nullptr;
    DropEntry drop = nullptr;
    std::shared_ptr<LunaJitModule> lease;
};

void executePrivateRefJitFragmentFrom(unsigned source, void* activation) {
    ++privateRefJitExecutions;
    if (source > 1) {
        privateRefJitPayloadValid = false;
        return;
    }
    const void* packed = luna_runtime_fragment_activation_arguments_v1(
        activation, privateRefJitExpectedSlotIds[source],
        privateRefJitExpectedSlotContracts[source],
        privateRefJitExpectedLayoutIds[source],
        privateRefJitExpectedSizes[source],
        privateRefJitExpectedAlignments[source]);
    if (!packed || privateRefJitExpectedSizes[source] < sizeof(int32_t)) {
        privateRefJitPayloadValid = false;
        return;
    }
    int32_t value = 0;
    std::memcpy(&value, packed, sizeof(value));
    privateRefJitObservedArguments.push_back(value);
    privateRefJitObservedSources.push_back(source);
    privateRefJitResumeStatus =
        luna_runtime_fragment_activation_resume_v1(activation);
    if (privateRefJitResumeStatus ==
        LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1)
        ++privateRefJitCompletedResumes;
}

void executePrivateRefJitFragment(void*, void* activation) {
    executePrivateRefJitFragmentFrom(0, activation);
}

void executePrivateRefJitFragmentSecond(void*, void* activation) {
    executePrivateRefJitFragmentFrom(1, activation);
}

bool exercisePrivateRefApplyJit(
    moon::Module& module, moon::FunctionDecl& function,
    std::string& error, unsigned expectedDispatches = 1,
    bool expectEscape = false,
    std::optional<unsigned> expectedCompletedDispatches = std::nullopt,
    bool twoBorrowedReferences = false, bool differentSlots = false,
    std::vector<int32_t> expectedValues = {},
    std::vector<unsigned> expectedSources = {},
    std::optional<std::pair<bool, int32_t>> expectedResult = std::nullopt,
    std::optional<unsigned> transferredDropCalls = std::nullopt,
    bool injectPostBodyFailure = false,
    bool deferSecondHostDrop = false,
    bool exerciseIngressGate = false,
    std::vector<uint8_t>* entryRecord = nullptr,
    unsigned bodyDropCalls = 0) {
    const moon::SlotDecl* slot = nullptr;
    const moon::SlotDecl* secondSlot = nullptr;
    for (const auto& declaration : module.declarations)
        if (const auto* candidate = dynamic_cast<const moon::SlotDecl*>(
                declaration.get()); candidate) {
            if (candidate->name == "checkpoint") slot = candidate;
            if (candidate->name == "shadow") secondSlot = candidate;
        }
    const auto* arguments = slot ? module.findType(slot->argumentsType) : nullptr;
    const auto* valueType = arguments && arguments->fields.size() == 1
        ? module.findType(arguments->fields.front().type) : nullptr;
    if (!slot || !arguments || (differentSlots && !secondSlot) ||
        (exerciseIngressGate && (!differentSlots || twoBorrowedReferences ||
                                 expectedResult || transferredDropCalls ||
                                 !entryRecord)) ||
        function.params.size() != (twoBorrowedReferences ? 2u : 1u) ||
        !valueType || valueType->kind != TypeKind::I32 ||
        std::any_of(function.params.begin(), function.params.end(),
            [](const auto& parameter) {
                return parameter.relation !=
                    luna::ownership::Relation::SharedBorrow;
            })) {
        error = "private Ref JIT fixture lacks Slot/layout/borrowed parameter";
        return false;
    }
    auto jit = CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
        module, function, error, entryRecord);
    if (!jit) return false;
    const void* address = jit->lookup(
        "__luna_private_ref_apply_jit_test", error);
    if (!address) return false;
    const void* transferAddress = transferredDropCalls
        ? jit->lookup("__luna_private_ref_apply_transfer_test", error)
        : nullptr;
    if (transferredDropCalls && !transferAddress) return false;
    const void* dropAddress = transferredDropCalls
        ? jit->lookup("__luna_private_ref_apply_drop_test", error)
        : nullptr;
    if (transferredDropCalls && !dropAddress) return false;
    std::unique_ptr<LunaPrivateRefUnitApplyLoadedEntry> loadedIngress;
    if (exerciseIngressGate) {
        loadedIngress = CodeGenerator::
            loadPrivateRuntimeFragmentRefApplyEntryForTest(
                module, function, *entryRecord, jit, error);
        if (!loadedIngress || loadedIngress->entryRecord() != *entryRecord)
            return false;
        auto forgedRecord = *entryRecord;
        forgedRecord[0] ^= 1;
        if (CodeGenerator::loadPrivateRuntimeFragmentRefApplyEntryForTest(
                module, function, forgedRecord, jit, error)) {
            error = "private Ref loaded entry accepted a forged record";
            return false;
        }
        auto unboundJit =
            CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
                module, function, error);
        if (!unboundJit ||
            CodeGenerator::loadPrivateRuntimeFragmentRefApplyEntryForTest(
                module, function, *entryRecord, unboundJit, error) ||
            error.find("not bound to this JIT module") ==
                std::string::npos) {
            error = "private Ref loaded entry accepted an unbound JIT module";
            return false;
        }
        unboundJit.reset();
        error.clear();
    }
    const bool needsSecondHandle = twoBorrowedReferences || exerciseIngressGate;

    const std::string fragmentId = "fragment:private-ref-jit";
    const std::string fragmentContract = "contract:private-ref-jit";
    const std::string secondFragmentId = "fragment:private-ref-jit-second";
    const std::string secondFragmentContract =
        "contract:private-ref-jit-second";
    const std::string unitLayout = "layout:unit";
    LunaRuntimeFragmentDescriptorV1 descriptor = {
        LUNA_RUNTIME_FRAGMENT_MAGIC_V1,
        LUNA_RUNTIME_FRAGMENT_ABI_V1,
        sizeof(LunaRuntimeFragmentDescriptorV1),
        LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1,
        0, 0, 0, 0,
        fragmentId.c_str(), fragmentContract.c_str(),
        slot->symbolId.value.c_str(), slot->contractId.value.c_str(),
        arguments->abiLayoutId.value.c_str(),
        arguments->valueSize, arguments->valueAlignment,
        "", unitLayout.c_str(), 0, 1,
        nullptr, nullptr, executePrivateRefJitFragment,
    };
    LunaRuntimeFragmentDescriptorV1 secondDescriptor = descriptor;
    secondDescriptor.fragment_id = secondFragmentId.c_str();
    secondDescriptor.fragment_contract_id = secondFragmentContract.c_str();
    secondDescriptor.execute = executePrivateRefJitFragmentSecond;
    const auto* secondArguments = differentSlots
        ? module.findType(secondSlot->argumentsType) : arguments;
    if (!secondArguments) {
        error = "private Ref JIT fixture lacks second Slot layout";
        return false;
    }
    if (differentSlots) {
        secondDescriptor.slot_id = secondSlot->symbolId.value.c_str();
        secondDescriptor.slot_contract_id = secondSlot->contractId.value.c_str();
        secondDescriptor.slot_arguments_layout_id =
            secondArguments->abiLayoutId.value.c_str();
        secondDescriptor.slot_arguments_size = secondArguments->valueSize;
        secondDescriptor.slot_arguments_alignment =
            secondArguments->valueAlignment;
    }
    if (!luna::runtime::validateRuntimeFragmentDescriptor(
            descriptor, error) ||
        (needsSecondHandle &&
         !luna::runtime::validateRuntimeFragmentDescriptor(
             secondDescriptor, error)))
        return false;

    luna::runtime::RuntimeFragmentRefHandle handle;
    luna::runtime::RuntimeFragmentRefHandle secondHandle;
    std::weak_ptr<int> generationLease;
    {
        luna::runtime::MoonRuntime runtime;
        auto lease = std::make_shared<int>(1);
        generationLease = lease;
        luna::runtime::GenerationStagingRequest request{
            "org.luna.private.ref-jit", std::string(64, 'a'), lease};
        luna::runtime::MoonRuntime::StagedGeneration staged;
        constexpr uint32_t flags =
            luna::runtime::GenerationBindingFragmentExecutable |
            luna::runtime::GenerationBindingPublicControl;
        if (!runtime.stage(
                request,
                [](const auto&, std::string&) { return true; },
                [&](const auto&, auto& bindings, std::string&) {
                    bindings.push_back({fragmentId, fragmentContract,
                        &descriptor, LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
                        flags});
                    if (needsSecondHandle)
                        bindings.push_back({secondFragmentId,
                            secondFragmentContract, &secondDescriptor,
                            LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, flags});
                    return true;
                }, {}, staged, error))
            return false;
        luna::runtime::MoonRuntime::PinnedGeneration loaded;
        if (!runtime.loadOnce(staged, loaded, error)) return false;
        const luna::runtime::GenerationBindingRequirement requirement{
            fragmentId, fragmentContract,
            LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, flags};
        const auto binding = loaded.find(requirement);
        luna::runtime::RuntimeFragmentRef reference;
        const luna::runtime::RuntimeSlotRequirement target{
            slot->symbolId.value, slot->contractId.value};
        if (!binding || !luna::runtime::makeOwnedRuntimeFragmentRef(
                binding, target, {"", nullptr}, reference, error) ||
            !luna::runtime::makeRuntimeFragmentRefHandle(
                reference, target, handle, error))
            return false;
        if (needsSecondHandle) {
            const luna::runtime::GenerationBindingRequirement secondRequirement{
                secondFragmentId, secondFragmentContract,
                LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, flags};
            const auto secondBinding = loaded.find(secondRequirement);
            luna::runtime::RuntimeFragmentRef secondReference;
            const auto* targetSlot = differentSlots ? secondSlot : slot;
            const luna::runtime::RuntimeSlotRequirement secondTarget{
                targetSlot->symbolId.value, targetSlot->contractId.value};
            if (!secondBinding ||
                !luna::runtime::makeOwnedRuntimeFragmentRef(
                    secondBinding, secondTarget, {"", nullptr}, secondReference,
                    error) ||
                !luna::runtime::makeRuntimeFragmentRefHandle(
                    secondReference, secondTarget, secondHandle, error))
                return false;
        }
    }
    if (!handle || (needsSecondHandle && !secondHandle) ||
        generationLease.expired()) {
        error = "private Ref JIT fixture lost its owning generation pin";
        return false;
    }
    luna::runtime::RuntimeFragmentBindingSet emptyBindings;
    std::vector<luna::runtime::RuntimeFragmentRef> none;
    luna::runtime::RuntimeFragmentExecutionContext parent;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            std::move(none), emptyBindings, error) ||
        !luna::runtime::makeRuntimeFragmentExecutionContext(
            emptyBindings, parent, error))
        return false;

    using EntryOne = void (*)(const void*, void*);
    using EntryTwo = void (*)(const void*, void*, void*);
    using EntryResultOne = uint64_t (*)(const void*, void*);
    static_assert(LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST == 0);
    static_assert(LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST == 1);
    static_assert(LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST == 2);
    static_assert(LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_TARGET_V1_TEST == 3);
    static_assert(LUNA_PRIVATE_REF_UNIT_APPLY_UNEXPECTED_CHECK_V1_TEST == 4);
    using EntryTransfer = int32_t (*)(
        const void*, void*, uint32_t*, void**, uint32_t);
    using EntryDrop = int32_t (*)(void**);
    const auto entryOne = reinterpret_cast<EntryOne>(
        const_cast<void*>(address));
    const auto entryTwo = reinterpret_cast<EntryTwo>(
        const_cast<void*>(address));
    const auto entryResultOne = reinterpret_cast<EntryResultOne>(
        const_cast<void*>(address));
    const auto entryTransfer = reinterpret_cast<EntryTransfer>(
        const_cast<void*>(transferAddress));
    const auto entryDrop = reinterpret_cast<EntryDrop>(
        const_cast<void*>(dropAddress));
    std::weak_ptr<LunaJitModule> codeLifetime = jit;
    if (exerciseIngressGate) {
        jit.reset();
        if (codeLifetime.expired()) {
            error = "private Ref loaded entry lost its JIT code lease";
            return false;
        }
    }
    std::optional<PrivateRefReturnedOwner> deferredOwner;
    unsigned deferredDropProbeBaseline = 0;
    unsigned transferInvocations = 0;
    const auto invoke = [&] {
        if (exerciseIngressGate) {
            const auto dispatchesBefore = privateRefJitExecutions;
            if (loadedIngress->call(nullptr,
                    const_cast<void*>(handle.opaque())) !=
                    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST ||
                loadedIngress->call(nullptr, nullptr) !=
                    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST ||
                loadedIngress->call(parent.opaque(), nullptr) !=
                    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST ||
                loadedIngress->call(parent.opaque(),
                    const_cast<void*>(secondHandle.opaque())) !=
                    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_TARGET_V1_TEST ||
                privateRefJitExecutions != dispatchesBefore ||
                loadedIngress->call(parent.opaque(),
                    const_cast<void*>(handle.opaque())) !=
                    LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST) {
                error = "private Ref host ingress bypassed context or exact target validation";
                return false;
            }
            return true;
        }
        if (transferredDropCalls) {
            const bool deferThisOwner =
                deferSecondHostDrop && ++transferInvocations == 2;
            if (!expectedResult || twoBorrowedReferences) {
                error = "private Result transfer fixture requires one borrowed Ref";
                return false;
            }
            const auto callsBefore = privateRefJitAllDropProbeCalls;
            const auto dispatchesBefore = privateRefJitExecutions;
            uint32_t tag = 42;
            void* occupied = reinterpret_cast<void*>(uintptr_t{1});
            if (entryTransfer(parent.opaque(),
                    const_cast<void*>(handle.opaque()), &tag, &occupied, 0) == 0 ||
                tag != 42 || occupied != reinterpret_cast<void*>(uintptr_t{1}) ||
                privateRefJitExecutions != dispatchesBefore ||
                privateRefJitAllDropProbeCalls != callsBefore) {
                error = "private Result transfer mutated an occupied owner cell";
                return false;
            }
            void* aliased = nullptr;
            void* partlyAliased = nullptr;
            if (entryTransfer(parent.opaque(),
                    const_cast<void*>(handle.opaque()),
                    reinterpret_cast<uint32_t*>(&aliased), &aliased, 0) == 0 ||
                aliased != nullptr ||
                entryTransfer(parent.opaque(),
                    const_cast<void*>(handle.opaque()),
                    reinterpret_cast<uint32_t*>(
                        reinterpret_cast<unsigned char*>(&partlyAliased) +
                        sizeof(uint32_t)), &partlyAliased, 0) == 0 ||
                partlyAliased != nullptr ||
                entryTransfer(parent.opaque(),
                    const_cast<void*>(handle.opaque()), nullptr, &aliased, 0) == 0 ||
                aliased != nullptr ||
                entryTransfer(parent.opaque(),
                    const_cast<void*>(handle.opaque()), &tag, nullptr, 0) == 0 ||
                tag != 42 || privateRefJitExecutions != dispatchesBefore ||
                privateRefJitAllDropProbeCalls != callsBefore ||
                entryDrop(nullptr) == 0) {
                error = "private Result transfer accepted invalid output storage";
                return false;
            }
            void* owner = nullptr;
            if (injectPostBodyFailure &&
                (entryTransfer(parent.opaque(),
                    const_cast<void*>(handle.opaque()), &tag, &owner, 1) != 3 ||
                 tag != 42 || owner != nullptr ||
                 privateRefJitAllDropProbeCalls !=
                     callsBefore + bodyDropCalls + *transferredDropCalls)) {
                error = "private Result failed transfer did not clean before commit";
                return false;
            }
            const auto callsAfterFailure = privateRefJitAllDropProbeCalls;
            if (entryTransfer(parent.opaque(),
                    const_cast<void*>(handle.opaque()), &tag, &owner, 0) != 0 ||
                tag != static_cast<uint32_t>(expectedResult->first) ||
                (owner != nullptr) != (*transferredDropCalls != 0) ||
                privateRefJitAllDropProbeCalls !=
                    callsAfterFailure + bodyDropCalls) {
                error = "private Result transfer failed its ownership commit";
                return false;
            }
            if (owner) {
                if (deferThisOwner) {
                    deferredOwner.emplace(owner, entryDrop, jit);
                    owner = nullptr;
                    deferredDropProbeBaseline = privateRefJitAllDropProbeCalls;
                    return true;
                }
                if (entryDrop(&owner) != 0 || owner != nullptr ||
                    privateRefJitAllDropProbeCalls !=
                        callsAfterFailure + bodyDropCalls +
                            *transferredDropCalls ||
                    entryDrop(&owner) == 0 ||
                    privateRefJitAllDropProbeCalls !=
                        callsAfterFailure + bodyDropCalls +
                            *transferredDropCalls) {
                    error = "private Result transferred owner was not dropped exactly once";
                    return false;
                }
            } else if (entryDrop(&owner) == 0 ||
                       privateRefJitAllDropProbeCalls !=
                           callsAfterFailure + bodyDropCalls) {
                error = "private Result scalar branch manufactured an owner";
                return false;
            }
            return true;
        }
        if (expectedResult) {
            if (twoBorrowedReferences) {
                error = "private Result JIT fixture requires one borrowed Ref";
                return false;
            }
            const uint64_t packed = entryResultOne(
                parent.opaque(), const_cast<void*>(handle.opaque()));
            const bool isOk = ((packed >> 32) & 1u) != 0;
            const int32_t payload = static_cast<int32_t>(
                static_cast<uint32_t>(packed));
            if (isOk != expectedResult->first ||
                payload != expectedResult->second) {
                error = "private Ref JIT returned the wrong Result variant or payload";
                return false;
            }
            return true;
        }
        if (twoBorrowedReferences)
            entryTwo(parent.opaque(), const_cast<void*>(handle.opaque()),
                const_cast<void*>(secondHandle.opaque()));
        else
            entryOne(parent.opaque(), const_cast<void*>(handle.opaque()));
        return true;
    };
    const auto before = privateRefJitExecutions;
    const auto resumesBefore = privateRefJitCompletedResumes;
    const auto argumentsBefore = privateRefJitObservedArguments.size();
    const auto sourcesBefore = privateRefJitObservedSources.size();
    privateRefJitSlotId = descriptor.slot_id;
    privateRefJitSlotContract = descriptor.slot_contract_id;
    privateRefJitLayoutId = descriptor.slot_arguments_layout_id;
    privateRefJitArgumentsSize = descriptor.slot_arguments_size;
    privateRefJitArgumentsAlignment = descriptor.slot_arguments_alignment;
    privateRefJitExpectedSlotIds = {descriptor.slot_id,
        secondDescriptor.slot_id};
    privateRefJitExpectedSlotContracts = {descriptor.slot_contract_id,
        secondDescriptor.slot_contract_id};
    privateRefJitExpectedLayoutIds = {descriptor.slot_arguments_layout_id,
        secondDescriptor.slot_arguments_layout_id};
    privateRefJitExpectedSizes = {descriptor.slot_arguments_size,
        secondDescriptor.slot_arguments_size};
    privateRefJitExpectedAlignments = {descriptor.slot_arguments_alignment,
        secondDescriptor.slot_arguments_alignment};
    privateRefJitPayloadValid = true;
    privateRefJitResumeStatus = -1;
    if (!invoke()) return false;
    const auto handleCheck = luna_runtime_fragment_ref_check_v1(
        handle.opaque(), slot->symbolId.value.c_str(),
        slot->contractId.value.c_str());
    const auto secondHandleCheck = needsSecondHandle
        ? luna_runtime_fragment_ref_check_v1(
            secondHandle.opaque(),
            (differentSlots ? secondSlot : slot)->symbolId.value.c_str(),
            (differentSlots ? secondSlot : slot)->contractId.value.c_str())
        : LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1;
    if (handleCheck != LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1 ||
        secondHandleCheck != LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1) {
        error = "private Ref JIT body consumed its borrowed Ref";
        return false;
    }
    if (!invoke()) return false;
    luna::runtime::RuntimeFragmentRefHandle callPin;
    luna::runtime::RuntimeFragmentExecutionContext parentPin;
    if (exerciseIngressGate) {
        if (!luna::runtime::pinRuntimeFragmentRefHandleForTest(
                handle, callPin, error))
            return false;
        parentPin = parent;
        if (luna::runtime::pinRuntimeFragmentRefHandleForTest(
                handle, callPin, error)) {
            error = "private Ref call pin overwrote a live output";
            return false;
        }
        error.clear();
    }
    handle.reset();
    secondHandle.reset();
    if (exerciseIngressGate) {
        parent = {};
        if (generationLease.expired() || !parentPin || !callPin ||
            loadedIngress->call(parentPin.opaque(), callPin.opaque()) !=
                LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST) {
            error = "private Ref call pin failed after original owners were released";
            return false;
        }
        callPin.reset();
        parentPin = {};
        if (!generationLease.expired()) {
            error = "private Ref call pin failed to release its generation";
            return false;
        }
    }
    const bool generationExpiredBeforeHostDrop = generationLease.expired();
    if (exerciseIngressGate) {
        loadedIngress.reset();
        if (!codeLifetime.expired()) {
            error = "private Ref loaded entry failed to release its JIT lease";
            return false;
        }
    }
    if (deferSecondHostDrop) {
        const auto callsBeforeDeferredDrop = privateRefJitAllDropProbeCalls;
        jit.reset();
        const bool codeLeaseRetained =
            deferredOwner && deferredOwner->live() &&
            !codeLifetime.expired();
        const int32_t dropStatus = deferredOwner
            ? deferredOwner->dropOnce() : 1;
        const int32_t repeatedDropStatus = deferredOwner
            ? deferredOwner->dropOnce() : 1;
        if (!generationExpiredBeforeHostDrop || !codeLeaseRetained ||
            callsBeforeDeferredDrop != deferredDropProbeBaseline ||
            dropStatus != 0 || repeatedDropStatus == 0 ||
            !deferredOwner || deferredOwner->live() ||
            !codeLifetime.expired() ||
            privateRefJitAllDropProbeCalls !=
                callsBeforeDeferredDrop + transferredDropCalls.value_or(0)) {
            error = "private Result owner did not outlive its Ref pin under a JIT lease";
            return false;
        }
    }
    const unsigned expectedCalls = exerciseIngressGate ? 3u : 2u;
    if (privateRefJitExecutions != before + expectedDispatches * expectedCalls ||
        privateRefJitCompletedResumes !=
            resumesBefore + expectedCompletedDispatches.value_or(
                expectEscape ? 0 : expectedDispatches) * expectedCalls ||
        privateRefJitResumeStatus != (expectedDispatches
            ? (expectEscape
                ? LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1
                : LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1)
            : -1) ||
        !privateRefJitPayloadValid ||
        privateRefJitObservedArguments.size() !=
            argumentsBefore + expectedDispatches * expectedCalls ||
        privateRefJitObservedSources.size() !=
            sourcesBefore + expectedDispatches * expectedCalls ||
        !generationLease.expired()) {
        std::ostringstream details;
        details << "private Ref JIT body did not dispatch and release its generation"
                << " (executions=" << privateRefJitExecutions - before
                << ", completed=" << privateRefJitCompletedResumes - resumesBefore
                << ", status=" << privateRefJitResumeStatus
                << ", payload=" << privateRefJitPayloadValid
                << ", arguments=" << privateRefJitObservedArguments.size() -
                       argumentsBefore
                << ", leaseExpired=" << generationLease.expired() << ')';
        error = details.str();
        return false;
    }
    if (expectedValues.empty())
        for (unsigned site = 0; site < expectedDispatches; ++site)
            expectedValues.push_back(static_cast<int32_t>(site + 1));
    if (expectedSources.empty())
        for (unsigned site = 0; site < expectedDispatches; ++site)
            expectedSources.push_back(twoBorrowedReferences && site == 1
                ? 1u : 0u);
    if (expectedValues.size() != expectedDispatches ||
        expectedSources.size() != expectedDispatches) {
        error = "private Ref JIT fixture has incomplete per-site expectations";
        return false;
    }
    for (unsigned repeat = 0; repeat < expectedCalls; ++repeat)
        for (unsigned site = 0; site < expectedDispatches; ++site)
            if (privateRefJitObservedArguments[
                    argumentsBefore + repeat * expectedDispatches + site] !=
                expectedValues[site]) {
                error = "private Ref JIT Slot sites executed out of order";
                return false;
            }
    for (unsigned repeat = 0; repeat < expectedCalls; ++repeat)
        for (unsigned site = 0; site < expectedDispatches; ++site) {
            if (privateRefJitObservedSources[
                    sourcesBefore + repeat * expectedDispatches + site] !=
                expectedSources[site]) {
                error = "private Ref nested apply dispatched from the wrong source";
                return false;
            }
        }
    error.clear();
    return true;
}

} // namespace

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
        "export slot shadow(value: i32);\n"
        "runtime fn host_entry(selected: RuntimeFragmentRef<checkpoint>) {\n"
        "  apply selected { checkpoint(1) {} }\n"
        "}\n",
        "<canonical-source-ref-apply-gate>");
    if (!sourceApplySnapshot.success())
        return fail("frontend rejected exact-Slot source Ref apply preparation");
    moon::LunaLowerer sourceApplyLowerer;
    auto sourceApplyModule = sourceApplyLowerer.lower(
        *sourceApplySnapshot.program(), *sourceApplySnapshot.symbolTable());
    if (!sourceApplyModule || !sourceApplyLowerer.errors().empty())
        return fail("source Ref apply did not lower to internal structured MoonIR");
    moon::FunctionDecl* sourceApplyEntry = nullptr;
    for (auto& declaration : sourceApplyModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function && function->name == "host_entry")
            sourceApplyEntry = function;
    moon::Sealer sourceApplySealer;
    if (!sourceApplyEntry ||
        !sourceApplySealer.sealFunctionBodies(*sourceApplyModule) ||
        !sourceApplyEntry->controlFlow ||
        sourceApplyEntry->controlFlow->runtimeRefApplies.size() != 1 ||
        !sourceApplyEntry->requiresFragmentContext)
        return fail("source Ref apply lost its internal CFG region or context effect");
    auto& refApplyBinding =
        sourceApplyEntry->controlFlow->runtimeRefApplies.front();
    const auto* refApplyLocal = sourceApplyEntry->controlFlow->findLocal(
        refApplyBinding.reference);
    const auto refApplyTarget = refApplyLocal
        ? sourceApplyModule->resolveRuntimeFragmentRefTarget(refApplyLocal->type)
        : std::nullopt;
    moon::Verifier refApplyCfgVerifier;
    if (!refApplyTarget || *refApplyTarget != refApplyBinding.slot ||
        !refApplyCfgVerifier.verify(*sourceApplyEntry->controlFlow,
                                    *sourceApplyModule))
        return fail("internal Ref apply CFG failed exact-target verification");
    std::string refApplyFlowError;
    const auto refApplyFlow = moon::planRuntimeRefApplyFlow(
        *sourceApplyEntry->controlFlow, refApplyFlowError);
    if (!refApplyFlow || !refApplyFlowError.empty() ||
        !std::any_of(refApplyFlow->edges.begin(), refApplyFlow->edges.end(),
                     [&](const auto& edge) {
                         return edge.enters ==
                             std::vector<moon::RegionId>{refApplyBinding.region};
                     }) ||
        !std::any_of(refApplyFlow->edges.begin(), refApplyFlow->edges.end(),
                     [&](const auto& edge) {
                         return edge.exits ==
                             std::vector<moon::RegionId>{refApplyBinding.region};
                     }))
        return fail("Ref apply flow lost normal context entry or exit");
    const auto* refApplyRegion = sourceApplyEntry->controlFlow->findRegion(
        refApplyBinding.region);
    moon::BasicBlock* enteringRefApply = nullptr;
    moon::BlockId nonEntryTarget;
    for (auto& block : sourceApplyEntry->controlFlow->blocks) {
        if (block.terminator.kind == moon::TerminatorKind::Jump &&
            refApplyRegion && block.terminator.primary.target ==
                refApplyRegion->entry)
            enteringRefApply = &block;
        if (block.terminator.kind == moon::TerminatorKind::RuntimeSlot)
            nonEntryTarget = block.terminator.primary.target;
    }
    if (!enteringRefApply || nonEntryTarget.empty())
        return fail("Ref apply flow fixture has no entry or nested continuation");
    const auto originalRefEntryTarget =
        enteringRefApply->terminator.primary.target;
    enteringRefApply->terminator.primary.target = nonEntryTarget;
    const bool bypassAccepted = moon::planRuntimeRefApplyFlow(
        *sourceApplyEntry->controlFlow, refApplyFlowError).has_value();
    const bool bypassVerified = refApplyCfgVerifier.verify(
        *sourceApplyEntry->controlFlow, *sourceApplyModule);
    enteringRefApply->terminator.primary.target = originalRefEntryTarget;
    if (bypassAccepted || bypassVerified || refApplyFlowError.find(
            "without its exact region entry") == std::string::npos)
        return fail("Ref apply flow accepted a bypassed context entry");
    const auto originalApplyContract = refApplyBinding.slot.contract;
    refApplyBinding.slot.contract = luna::identity::contractIdFromCanonical(
        "forged Ref apply Slot contract");
    const bool forgedRefApplyAccepted = refApplyCfgVerifier.verify(
        *sourceApplyEntry->controlFlow, *sourceApplyModule);
    refApplyBinding.slot.contract = originalApplyContract;
    if (forgedRefApplyAccepted)
        return fail("internal Ref apply CFG accepted a retargeted Slot contract");
    const auto originalApplyLocal = refApplyBinding.reference;
    refApplyBinding.reference = moon::LocalId{};
    const bool missingRefLocalAccepted = refApplyCfgVerifier.verify(
        *sourceApplyEntry->controlFlow, *sourceApplyModule);
    refApplyBinding.reference = originalApplyLocal;
    if (missingRefLocalAccepted)
        return fail("internal Ref apply CFG accepted a missing owner local");
    sourceApplyEntry->controlFlow->runtimeRefApplies.push_back(refApplyBinding);
    const bool duplicateRefApplyAccepted = refApplyCfgVerifier.verify(
        *sourceApplyEntry->controlFlow, *sourceApplyModule);
    sourceApplyEntry->controlFlow->runtimeRefApplies.pop_back();
    if (duplicateRefApplyAccepted)
        return fail("internal Ref apply CFG accepted duplicate region ownership");
    std::vector<uint8_t> blockedApplyCode{1, 2, 3};
    std::string blockedApplyWireError;
    if (moon::ContainerModelCodec::encodeCode(
            *sourceApplyModule, blockedApplyCode, blockedApplyWireError) ||
        !blockedApplyCode.empty() ||
        blockedApplyWireError.find(
            "cannot encode internal RuntimeFragmentRef apply regions") ==
            std::string::npos)
        return fail("internal Ref apply CFG escaped the frozen code wire gate");
    if (verifier.verify(*sourceApplyModule) ||
        !std::any_of(verifier.errors().begin(), verifier.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "RuntimeFragmentRef apply context override is internal-only") !=
                             std::string::npos;
                     }))
        return fail("internal Ref apply CFG escaped module publication gating");
    CodeGenerator blockedSourceApply("canonical-ref-apply-codegen-gate");
    if (blockedSourceApply.generate(sourceApplyModule.get()) ||
        !std::any_of(blockedSourceApply.errors().begin(),
                     blockedSourceApply.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "1 private Ref apply body(s) verified and discarded") !=
                             std::string::npos;
                     })) {
        for (const auto& diagnostic : blockedSourceApply.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("private Ref apply body proof or public codegen gate failed");
    }
    std::string privateRefJitError;
    std::vector<uint8_t> privateIngressRecord;
    if (!exercisePrivateRefApplyJit(
            *sourceApplyModule, *sourceApplyEntry, privateRefJitError,
            1, false, std::nullopt, false, true, {}, {},
            std::nullopt, std::nullopt, false, false, true,
            &privateIngressRecord)) {
        std::cerr << privateRefJitError << '\n';
        return fail("normal-exit source Ref apply failed private JIT execution");
    }
    const auto validPrivateIngressRecord =
        [&](const std::vector<uint8_t>& bytes) {
            std::string reason;
            return CodeGenerator::
                validatePrivateRuntimeFragmentRefApplyEntryRecordForTest(
                    *sourceApplyModule, *sourceApplyEntry, bytes, reason);
        };
    if (!validPrivateIngressRecord(privateIngressRecord))
        return fail("private Ref Apply entry record did not round-trip");
    const auto readPrivateU32 = [](const std::vector<uint8_t>& bytes,
                                   size_t offset) -> uint32_t {
        uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index)
            value |= static_cast<uint32_t>(bytes[offset + index]) <<
                (8 * index);
        return value;
    };
    const auto writePrivateU32 = [](std::vector<uint8_t>& bytes,
                                    size_t offset, uint32_t value) {
        for (unsigned index = 0; index < 4; ++index)
            bytes[offset + index] =
                static_cast<uint8_t>(value >> (8 * index));
    };
    if (privateIngressRecord.size() < 32 ||
        readPrivateU32(privateIngressRecord, 20) != 11 ||
        readPrivateU32(privateIngressRecord, 24) != 9)
        return fail("private Ref Apply entry record lost its bounded field layout");
    const auto rejectedPrivateIngressMutation = [&](size_t offset,
                                                     uint32_t value) {
        auto mutated = privateIngressRecord;
        writePrivateU32(mutated, offset, value);
        return !validPrivateIngressRecord(mutated);
    };
    if (!rejectedPrivateIngressMutation(8, 0) ||
        !rejectedPrivateIngressMutation(12,
            static_cast<uint32_t>(privateIngressRecord.size() - 1)) ||
        !rejectedPrivateIngressMutation(16, 1) ||
        !rejectedPrivateIngressMutation(20, 10) ||
        !rejectedPrivateIngressMutation(20, 12) ||
        !rejectedPrivateIngressMutation(24, 8) ||
        !rejectedPrivateIngressMutation(24, 10) ||
        !rejectedPrivateIngressMutation(28, 4097))
        return fail("private Ref Apply entry accepted an invalid header or length");
    auto malformedPrivateIngress = privateIngressRecord;
    malformedPrivateIngress[0] = 'X';
    if (validPrivateIngressRecord(malformedPrivateIngress))
        return fail("private Ref Apply entry accepted a different record magic");
    malformedPrivateIngress = privateIngressRecord;
    malformedPrivateIngress.resize(30);
    writePrivateU32(malformedPrivateIngress, 12,
        static_cast<uint32_t>(malformedPrivateIngress.size()));
    if (validPrivateIngressRecord(malformedPrivateIngress))
        return fail("private Ref Apply entry accepted a truncated field length");
    malformedPrivateIngress = privateIngressRecord;
    malformedPrivateIngress.resize(
        32 + readPrivateU32(privateIngressRecord, 28) - 1);
    writePrivateU32(malformedPrivateIngress, 12,
        static_cast<uint32_t>(malformedPrivateIngress.size()));
    if (validPrivateIngressRecord(malformedPrivateIngress))
        return fail("private Ref Apply entry accepted a truncated field value");
    malformedPrivateIngress = privateIngressRecord;
    malformedPrivateIngress.push_back(0);
    writePrivateU32(malformedPrivateIngress, 12,
        static_cast<uint32_t>(malformedPrivateIngress.size()));
    if (validPrivateIngressRecord(malformedPrivateIngress))
        return fail("private Ref Apply entry accepted trailing bytes");
    size_t privateFieldCursor = 28;
    for (unsigned field = 0; field < 11; ++field) {
        if (privateFieldCursor + 4 > privateIngressRecord.size())
            return fail("private Ref Apply entry has a truncated identity length");
        const uint32_t length =
            readPrivateU32(privateIngressRecord, privateFieldCursor);
        privateFieldCursor += 4;
        if (length == 0 || privateFieldCursor > privateIngressRecord.size() ||
            length > privateIngressRecord.size() - privateFieldCursor)
            return fail("private Ref Apply entry has a malformed identity field");
        malformedPrivateIngress = privateIngressRecord;
        malformedPrivateIngress[privateFieldCursor] =
            malformedPrivateIngress[privateFieldCursor] == 'X' ? 'Y' : 'X';
        if (validPrivateIngressRecord(malformedPrivateIngress))
            return fail("private Ref Apply entry accepted a changed identity");
        privateFieldCursor += length;
    }
    for (unsigned field = 0; field < 9; ++field) {
        if (privateFieldCursor + 4 > privateIngressRecord.size() ||
            !rejectedPrivateIngressMutation(privateFieldCursor,
                readPrivateU32(privateIngressRecord, privateFieldCursor) + 1) ||
            (field == 8 &&
             !rejectedPrivateIngressMutation(privateFieldCursor, 0)))
            return fail("private Ref Apply entry accepted a changed ABI convention");
        privateFieldCursor += 4;
    }
    if (privateFieldCursor != privateIngressRecord.size())
        return fail("private Ref Apply entry has an unaccounted field");
    LunaNativeExportDescriptorV1 genericNativeV1{};
    genericNativeV1.abi_version = LUNA_NATIVE_DESCRIPTOR_ABI_V1;
    genericNativeV1.struct_size = sizeof(genericNativeV1);
    genericNativeV1.declaration_kind =
        LUNA_NATIVE_DECLARATION_FUNCTION_V1;
    genericNativeV1.flags = LUNA_NATIVE_EXPORT_CALLABLE_V1;
    const auto* genericBytes =
        reinterpret_cast<const uint8_t*>(&genericNativeV1);
    if (validPrivateIngressRecord(std::vector<uint8_t>(
            genericBytes, genericBytes + sizeof(genericNativeV1))))
        return fail("private Ref Apply entry interpreted a generic Native v1 row");
    auto* ingressRecord = const_cast<moon::DeclarationRecord*>(
        sourceApplyModule->findDeclarationById(sourceApplyEntry->declarationId));
    auto* ingressCallable = ingressRecord
        ? const_cast<moon::TypeRecord*>(
            sourceApplyModule->findType(ingressRecord->type)) : nullptr;
    if (!ingressRecord || !ingressCallable ||
        ingressCallable->parameterContracts.size() != 1)
        return fail("private Ref Apply entry lacks a frozen callable record");
    const auto originalIngressLinkage = ingressRecord->linkageName;
    ingressRecord->linkageName += ".forged";
    const bool forgedIngressRecordAccepted =
        validPrivateIngressRecord(privateIngressRecord);
    const bool forgedIngressLinkageAccepted =
        static_cast<bool>(CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
            *sourceApplyModule, *sourceApplyEntry, privateRefJitError));
    ingressRecord->linkageName = originalIngressLinkage;
    const auto originalIngressContract =
        ingressCallable->parameterContracts.front();
    ingressCallable->parameterContracts.front().relation =
        luna::ownership::Relation::Owned;
    const bool forgedBorrowRecordAccepted =
        validPrivateIngressRecord(privateIngressRecord);
    const bool forgedIngressBorrowAccepted =
        static_cast<bool>(CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
            *sourceApplyModule, *sourceApplyEntry, privateRefJitError));
    ingressCallable->parameterContracts.front() = originalIngressContract;
    sourceApplyEntry->requiresFragmentContext = false;
    const bool forgedContextRecordAccepted =
        validPrivateIngressRecord(privateIngressRecord);
    const bool forgedContextJitAccepted =
        static_cast<bool>(CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
            *sourceApplyModule, *sourceApplyEntry, privateRefJitError));
    sourceApplyEntry->requiresFragmentContext = true;
    if (forgedIngressRecordAccepted || forgedIngressLinkageAccepted ||
        forgedBorrowRecordAccepted || forgedIngressBorrowAccepted ||
        forgedContextRecordAccepted || forgedContextJitAccepted)
        return fail("private Ref Apply ingress accepted forged frozen entry facts");
    auto sequentialSlotsSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn sequence(selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected { checkpoint(1) {} checkpoint(2) {} }\n"
            "}\n",
            "<canonical-ref-sequential-slots>");
    if (!sequentialSlotsSnapshot.success())
        return fail("frontend rejected sequential Ref apply Slot sites");
    moon::LunaLowerer sequentialSlotsLowerer;
    auto sequentialSlotsModule = sequentialSlotsLowerer.lower(
        *sequentialSlotsSnapshot.program(),
        *sequentialSlotsSnapshot.symbolTable());
    moon::Sealer sequentialSlotsSealer;
    if (!sequentialSlotsModule || !sequentialSlotsLowerer.errors().empty() ||
        !sequentialSlotsSealer.sealFunctionBodies(*sequentialSlotsModule))
        return fail("sequential Ref apply Slot sites did not seal");
    moon::FunctionDecl* sequence = nullptr;
    for (auto& declaration : sequentialSlotsModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function && function->name == "sequence")
            sequence = function;
    if (!sequence || !sequence->controlFlow ||
        std::count_if(sequence->controlFlow->blocks.begin(),
                      sequence->controlFlow->blocks.end(),
                      [](const auto& block) {
                          return block.terminator.kind ==
                              moon::TerminatorKind::RuntimeSlot;
                      }) != 2)
        return fail("sequential Ref apply lost one of its Slot CFG sites");
    CodeGenerator blockedSequentialSlots("canonical-ref-sequential-slots-gate");
    if (blockedSequentialSlots.generate(sequentialSlotsModule.get()) ||
        !std::any_of(blockedSequentialSlots.errors().begin(),
                     blockedSequentialSlots.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "1 private Ref apply body(s) verified and discarded") !=
                             std::string::npos;
                     }))
        return fail("sequential Ref apply Slot proof or public gate failed");
    if (!exercisePrivateRefApplyJit(
            *sequentialSlotsModule, *sequence, privateRefJitError, 2)) {
        std::cerr << privateRefJitError << '\n';
        return fail("sequential Ref apply Slots failed private JIT execution");
    }
    auto outlinedSlotSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn outlined(selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected { checkpoint(1) { checkpoint(2) {} } }\n"
            "}\n",
            "<canonical-ref-outlined-slot>");
    if (!outlinedSlotSnapshot.success())
        return fail("frontend rejected outlined Ref apply Slot site");
    moon::LunaLowerer outlinedSlotLowerer;
    auto outlinedSlotModule = outlinedSlotLowerer.lower(
        *outlinedSlotSnapshot.program(),
        *outlinedSlotSnapshot.symbolTable());
    moon::Sealer outlinedSlotSealer;
    if (!outlinedSlotModule || !outlinedSlotLowerer.errors().empty() ||
        !outlinedSlotSealer.sealFunctionBodies(*outlinedSlotModule))
        return fail("outlined Ref apply Slot site did not seal");
    moon::FunctionDecl* outlined = nullptr;
    for (auto& declaration : outlinedSlotModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function && function->name == "outlined")
            outlined = function;
    if (!outlined || !exercisePrivateRefApplyJit(
            *outlinedSlotModule, *outlined, privateRefJitError, 2)) {
        std::cerr << privateRefJitError << '\n';
        return fail("outlined Ref apply Slot failed private JIT execution");
    }
    auto recursiveSlotSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn recursive(selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected { checkpoint(1) { checkpoint(2) { checkpoint(3) {} } } }\n"
            "}\n",
            "<canonical-ref-recursive-slot>");
    if (!recursiveSlotSnapshot.success())
        return fail("frontend rejected recursive Ref apply Slot sites");
    moon::LunaLowerer recursiveSlotLowerer;
    auto recursiveSlotModule = recursiveSlotLowerer.lower(
        *recursiveSlotSnapshot.program(),
        *recursiveSlotSnapshot.symbolTable());
    moon::Sealer recursiveSlotSealer;
    if (!recursiveSlotModule || !recursiveSlotLowerer.errors().empty() ||
        !recursiveSlotSealer.sealFunctionBodies(*recursiveSlotModule))
        return fail("recursive Ref apply Slot sites did not seal");
    moon::FunctionDecl* recursive = nullptr;
    for (auto& declaration : recursiveSlotModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function && function->name == "recursive")
            recursive = function;
    if (!recursive || !exercisePrivateRefApplyJit(
            *recursiveSlotModule, *recursive, privateRefJitError, 3)) {
        std::cerr << privateRefJitError << '\n';
        return fail("recursive Ref apply Slots failed private JIT execution");
    }
    auto nestedApplyJumpSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn nested_apply_jump("
                "selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected {\n"
            "    checkpoint(1) {}\n"
            "    apply selected { checkpoint(2) {} }\n"
            "    checkpoint(3) {}\n"
            "  }\n"
            "}\n",
            "<canonical-ref-nested-apply-jump>");
    if (!nestedApplyJumpSnapshot.success())
        return fail("frontend rejected same-Ref nested apply jump fixture");
    moon::LunaLowerer nestedApplyJumpLowerer;
    auto nestedApplyJumpModule = nestedApplyJumpLowerer.lower(
        *nestedApplyJumpSnapshot.program(),
        *nestedApplyJumpSnapshot.symbolTable());
    moon::Sealer nestedApplyJumpSealer;
    if (!nestedApplyJumpModule || !nestedApplyJumpLowerer.errors().empty() ||
        !nestedApplyJumpSealer.sealFunctionBodies(*nestedApplyJumpModule))
        return fail("same-Ref nested apply jump fixture did not seal");
    moon::FunctionDecl* nestedApplyJump = nullptr;
    for (auto& declaration : nestedApplyJumpModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "nested_apply_jump")
            nestedApplyJump = function;
    if (!nestedApplyJump || !exercisePrivateRefApplyJit(
            *nestedApplyJumpModule, *nestedApplyJump,
            privateRefJitError, 3)) {
        std::cerr << privateRefJitError << '\n';
        return fail("same-Ref nested apply jump failed private JIT execution");
    }
    auto nestedApplyReturnSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn nested_apply_return("
                "selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected {\n"
            "    checkpoint(1) {}\n"
            "    apply selected { checkpoint(2) { return; } }\n"
            "    checkpoint(3) {}\n"
            "  }\n"
            "}\n",
            "<canonical-ref-nested-apply-return>");
    if (!nestedApplyReturnSnapshot.success())
        return fail("frontend rejected same-Ref nested apply return fixture");
    moon::LunaLowerer nestedApplyReturnLowerer;
    auto nestedApplyReturnModule = nestedApplyReturnLowerer.lower(
        *nestedApplyReturnSnapshot.program(),
        *nestedApplyReturnSnapshot.symbolTable());
    moon::Sealer nestedApplyReturnSealer;
    if (!nestedApplyReturnModule ||
        !nestedApplyReturnLowerer.errors().empty() ||
        !nestedApplyReturnSealer.sealFunctionBodies(*nestedApplyReturnModule))
        return fail("same-Ref nested apply return fixture did not seal");
    moon::FunctionDecl* nestedApplyReturn = nullptr;
    for (auto& declaration : nestedApplyReturnModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "nested_apply_return")
            nestedApplyReturn = function;
    if (!nestedApplyReturn || !exercisePrivateRefApplyJit(
            *nestedApplyReturnModule, *nestedApplyReturn,
            privateRefJitError, 2, true, 1)) {
        std::cerr << privateRefJitError << '\n';
        return fail("same-Ref nested apply return failed private JIT execution");
    }
    auto crossRefNestedApplySnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn nested_apply_cross_ref("
                "outer: RuntimeFragmentRef<checkpoint>, "
                "inner: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply outer {\n"
            "    checkpoint(1) {}\n"
            "    apply inner { checkpoint(2) {} }\n"
            "    checkpoint(3) {}\n"
            "  }\n"
            "}\n",
            "<canonical-ref-nested-apply-cross-ref>");
    if (!crossRefNestedApplySnapshot.success())
        return fail("frontend rejected different-Ref nested apply fixture");
    moon::LunaLowerer crossRefNestedApplyLowerer;
    auto crossRefNestedApplyModule = crossRefNestedApplyLowerer.lower(
        *crossRefNestedApplySnapshot.program(),
        *crossRefNestedApplySnapshot.symbolTable());
    moon::Sealer crossRefNestedApplySealer;
    if (!crossRefNestedApplyModule ||
        !crossRefNestedApplyLowerer.errors().empty() ||
        !crossRefNestedApplySealer.sealFunctionBodies(
            *crossRefNestedApplyModule))
        return fail("different-Ref nested apply fixture did not seal");
    moon::FunctionDecl* crossRefNestedApply = nullptr;
    for (auto& declaration : crossRefNestedApplyModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "nested_apply_cross_ref")
            crossRefNestedApply = function;
    if (!crossRefNestedApply)
        return fail("different-Ref nested apply function is missing");
    if (!exercisePrivateRefApplyJit(
            *crossRefNestedApplyModule, *crossRefNestedApply,
            privateRefJitError, 3, false, std::nullopt, true)) {
        std::cerr << privateRefJitError << '\n';
        return fail("different-Ref nested apply failed private JIT execution");
    }
    auto crossSlotApplySnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "export slot shadow(value: i32);\n"
            "runtime fn nested_apply_cross_slot("
                "outer: RuntimeFragmentRef<checkpoint>, "
                "inner: RuntimeFragmentRef<shadow>) {\n"
            "  apply outer {\n"
            "    checkpoint(1) {}\n"
            "    apply inner { shadow(2) {} checkpoint(3) {} }\n"
            "    checkpoint(4) {}\n"
            "  }\n"
            "}\n",
            "<canonical-ref-nested-apply-cross-slot>");
    if (!crossSlotApplySnapshot.success())
        return fail("frontend rejected the different-Slot nested apply fixture");
    moon::LunaLowerer crossSlotApplyLowerer;
    auto crossSlotApplyModule = crossSlotApplyLowerer.lower(
        *crossSlotApplySnapshot.program(), *crossSlotApplySnapshot.symbolTable());
    moon::Sealer crossSlotApplySealer;
    if (!crossSlotApplyModule || !crossSlotApplyLowerer.errors().empty() ||
        !crossSlotApplySealer.sealFunctionBodies(*crossSlotApplyModule))
        return fail("different-Slot nested apply fixture did not seal");
    moon::FunctionDecl* crossSlotApply = nullptr;
    for (auto& declaration : crossSlotApplyModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "nested_apply_cross_slot")
            crossSlotApply = function;
    std::string crossSlotGateError;
    if (!crossSlotApply || !exercisePrivateRefApplyJit(
            *crossSlotApplyModule, *crossSlotApply, crossSlotGateError,
            4, false, std::nullopt, true, true,
            {1, 2, 3, 4}, {0, 1, 0, 0})) {
        std::cerr << crossSlotGateError << '\n';
        return fail("different-Slot nested apply failed private JIT execution");
    }
    auto crossSlotReturnSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "export slot shadow(value: i32);\n"
            "runtime fn nested_apply_cross_slot_return("
                "outer: RuntimeFragmentRef<checkpoint>, "
                "inner: RuntimeFragmentRef<shadow>) {\n"
            "  apply outer {\n"
            "    checkpoint(1) {}\n"
            "    apply inner { shadow(2) { return; } }\n"
            "    checkpoint(3) {}\n"
            "  }\n"
            "}\n",
            "<canonical-ref-nested-apply-cross-slot-return>");
    if (!crossSlotReturnSnapshot.success())
        return fail("frontend rejected different-Slot nested apply return");
    moon::LunaLowerer crossSlotReturnLowerer;
    auto crossSlotReturnModule = crossSlotReturnLowerer.lower(
        *crossSlotReturnSnapshot.program(), *crossSlotReturnSnapshot.symbolTable());
    moon::Sealer crossSlotReturnSealer;
    if (!crossSlotReturnModule || !crossSlotReturnLowerer.errors().empty() ||
        !crossSlotReturnSealer.sealFunctionBodies(*crossSlotReturnModule))
        return fail("different-Slot nested apply return did not seal");
    moon::FunctionDecl* crossSlotReturn = nullptr;
    for (auto& declaration : crossSlotReturnModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "nested_apply_cross_slot_return")
            crossSlotReturn = function;
    if (!crossSlotReturn || !exercisePrivateRefApplyJit(
            *crossSlotReturnModule, *crossSlotReturn, privateRefJitError,
            2, true, 1, true, true, {1, 2}, {0, 1})) {
        std::cerr << privateRefJitError << '\n';
        return fail("different-Slot nested return failed private JIT execution");
    }
    auto callbackApplySnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn callback_apply("
                "selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected {\n"
            "    checkpoint(1) {\n"
            "      apply selected { checkpoint(2) { checkpoint(3) {} } }\n"
            "    }\n"
            "    checkpoint(4) {}\n"
            "  }\n"
            "}\n",
            "<canonical-ref-callback-apply>");
    if (!callbackApplySnapshot.success())
        return fail("frontend rejected callback-nested Ref apply fixture");
    moon::LunaLowerer callbackApplyLowerer;
    auto callbackApplyModule = callbackApplyLowerer.lower(
        *callbackApplySnapshot.program(),
        *callbackApplySnapshot.symbolTable());
    moon::Sealer callbackApplySealer;
    if (!callbackApplyModule || !callbackApplyLowerer.errors().empty() ||
        !callbackApplySealer.sealFunctionBodies(*callbackApplyModule))
        return fail("callback-nested Ref apply fixture did not seal");
    moon::FunctionDecl* callbackApply = nullptr;
    for (auto& declaration : callbackApplyModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "callback_apply")
            callbackApply = function;
    if (!callbackApply || !exercisePrivateRefApplyJit(
            *callbackApplyModule, *callbackApply, privateRefJitError,
            4, false, std::nullopt, false, false,
            {1, 2, 3, 4}, {0, 0, 0, 0})) {
        std::cerr << privateRefJitError << '\n';
        return fail("callback-nested Ref apply failed private JIT execution");
    }
    auto callbackApplyReturnSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn callback_apply_return("
                "selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected {\n"
            "    checkpoint(1) {\n"
            "      apply selected { checkpoint(2) { return; } }\n"
            "      checkpoint(3) {}\n"
            "    }\n"
            "    checkpoint(4) {}\n"
            "  }\n"
            "}\n",
            "<canonical-ref-callback-apply-return>");
    if (!callbackApplyReturnSnapshot.success())
        return fail("frontend rejected callback-nested Ref apply return fixture");
    moon::LunaLowerer callbackApplyReturnLowerer;
    auto callbackApplyReturnModule = callbackApplyReturnLowerer.lower(
        *callbackApplyReturnSnapshot.program(),
        *callbackApplyReturnSnapshot.symbolTable());
    moon::Sealer callbackApplyReturnSealer;
    if (!callbackApplyReturnModule ||
        !callbackApplyReturnLowerer.errors().empty() ||
        !callbackApplyReturnSealer.sealFunctionBodies(
            *callbackApplyReturnModule))
        return fail("callback-nested Ref apply return fixture did not seal");
    moon::FunctionDecl* callbackApplyReturn = nullptr;
    for (auto& declaration : callbackApplyReturnModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "callback_apply_return")
            callbackApplyReturn = function;
    if (!callbackApplyReturn || !exercisePrivateRefApplyJit(
            *callbackApplyReturnModule, *callbackApplyReturn,
            privateRefJitError, 2, true, 0, false, false,
            {1, 2}, {0, 0})) {
        std::cerr << privateRefJitError << '\n';
        return fail("callback-nested Ref apply return failed private JIT execution");
    }
    auto outlinedReturnSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn outlined_return(selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected { checkpoint(1) { return; } checkpoint(2) {} }\n"
            "}\n",
            "<canonical-ref-outlined-return>");
    if (!outlinedReturnSnapshot.success())
        return fail("frontend rejected outlined Ref apply return fixture");
    moon::LunaLowerer outlinedReturnLowerer;
    auto outlinedReturnModule = outlinedReturnLowerer.lower(
        *outlinedReturnSnapshot.program(),
        *outlinedReturnSnapshot.symbolTable());
    moon::Sealer outlinedReturnSealer;
    if (!outlinedReturnModule || !outlinedReturnLowerer.errors().empty() ||
        !outlinedReturnSealer.sealFunctionBodies(*outlinedReturnModule))
        return fail("outlined Ref apply return fixture did not seal");
    moon::FunctionDecl* outlinedReturn = nullptr;
    for (auto& declaration : outlinedReturnModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "outlined_return")
            outlinedReturn = function;
    if (!outlinedReturn)
        return fail("outlined Ref apply return fixture lost its function");
    if (!exercisePrivateRefApplyJit(
            *outlinedReturnModule, *outlinedReturn, privateRefJitError,
            1, true)) {
        std::cerr << privateRefJitError << '\n';
        return fail("outlined Ref apply return failed private JIT execution");
    }
    auto nestedOutlinedReturnSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn nested_outlined_return("
                "selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected { checkpoint(1) { checkpoint(2) { return; } } "
                "checkpoint(3) {} }\n"
            "}\n",
            "<canonical-ref-nested-outlined-return>");
    if (!nestedOutlinedReturnSnapshot.success())
        return fail("frontend rejected nested outlined Ref return fixture");
    moon::LunaLowerer nestedOutlinedReturnLowerer;
    auto nestedOutlinedReturnModule = nestedOutlinedReturnLowerer.lower(
        *nestedOutlinedReturnSnapshot.program(),
        *nestedOutlinedReturnSnapshot.symbolTable());
    moon::Sealer nestedOutlinedReturnSealer;
    if (!nestedOutlinedReturnModule ||
        !nestedOutlinedReturnLowerer.errors().empty() ||
        !nestedOutlinedReturnSealer.sealFunctionBodies(
            *nestedOutlinedReturnModule))
        return fail("nested outlined Ref return fixture did not seal");
    moon::FunctionDecl* nestedOutlinedReturn = nullptr;
    for (auto& declaration : nestedOutlinedReturnModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "nested_outlined_return")
            nestedOutlinedReturn = function;
    if (!nestedOutlinedReturn || !exercisePrivateRefApplyJit(
            *nestedOutlinedReturnModule, *nestedOutlinedReturn,
            privateRefJitError, 2, true)) {
        std::cerr << privateRefJitError << '\n';
        return fail("nested outlined Ref return failed private JIT execution");
    }

    auto sourceEarlyReturnSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn early(selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected { apply selected { return; } }\n"
            "}\n",
            "<canonical-nested-ref-early-return>");
    if (!sourceEarlyReturnSnapshot.success())
        return fail("frontend rejected nested Ref apply early-return fixture");
    moon::LunaLowerer earlyReturnLowerer;
    auto earlyReturnModule = earlyReturnLowerer.lower(
        *sourceEarlyReturnSnapshot.program(),
        *sourceEarlyReturnSnapshot.symbolTable());
    moon::Sealer earlyReturnSealer;
    if (!earlyReturnModule || !earlyReturnLowerer.errors().empty() ||
        !earlyReturnSealer.sealFunctionBodies(*earlyReturnModule))
        return fail("nested Ref apply early return did not seal");
    moon::FunctionDecl* earlyReturn = nullptr;
    for (auto& declaration : earlyReturnModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function && function->name == "early")
            earlyReturn = function;
    if (!earlyReturn || !earlyReturn->controlFlow ||
        earlyReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*earlyReturn->controlFlow,
                                    *earlyReturnModule))
        return fail("nested Ref apply early return failed CFG verification");
    const auto earlyFlow = moon::planRuntimeRefApplyFlow(
        *earlyReturn->controlFlow, refApplyFlowError);
    if (!earlyFlow || earlyFlow->terminals.size() != 1 ||
        earlyFlow->terminals.front().kind != moon::TerminatorKind::Return ||
        earlyFlow->terminals.front().exits !=
            std::vector<moon::RegionId>{
                earlyReturn->controlFlow->runtimeRefApplies[1].region,
                earlyReturn->controlFlow->runtimeRefApplies[0].region})
        return fail("nested Ref apply return lost inner-before-outer cleanup order");
    auto tryApplySnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        "extern \"C\" fn luna_private_ref_drop_probe(marker: i32) -> unit;\n"
        "struct ApplyLocal { marker: i32; }\n"
        "impl Drop for ApplyLocal {\n"
        "  fn drop(resource: &mut ApplyLocal) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "  }\n"
        "}\n"
        "struct SourceError { marker: i32; }\n"
        "impl Drop for SourceError {\n"
        "  fn drop(resource: &mut SourceError) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "  }\n"
        "}\n"
        "impl From<SourceError> for i32 {\n"
        "  fn from(affine error: SourceError) -> i32 {\n"
        "    return error.marker;\n"
        "  }\n"
        "}\n"
        "struct SourceNestedError { marker: i32; inner: SourceError; }\n"
        "impl Drop for SourceNestedError {\n"
        "  fn drop(resource: &mut SourceNestedError) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "  }\n"
        "}\n"
        "impl From<SourceNestedError> for i32 {\n"
        "  fn from(affine error: SourceNestedError) -> i32 {\n"
        "    if error.marker > 0 {\n"
        "      let forwarded = move error;\n"
        "      return forwarded.marker;\n"
        "    }\n"
        "    return 0 - error.marker;\n"
        "  }\n"
        "}\n"
        "struct SourceSplitError { first: SourceError; second: SourceError; }\n"
        "impl From<SourceSplitError> for i32 {\n"
        "  fn from(affine error: SourceSplitError) -> i32 {\n"
        "    if error.first.marker > 0 {\n"
        "      let forwarded = move error.first;\n"
        "      return forwarded.marker;\n"
        "    }\n"
        "    let forwarded = move error.second;\n"
        "    return forwarded.marker;\n"
        "  }\n"
        "}\n"
        "struct SourceMergeError { first: SourceError; second: SourceError; }\n"
        "impl From<SourceMergeError> for i32 {\n"
        "  fn from(affine error: SourceMergeError) -> i32 {\n"
        "    if error.first.marker > 0 {\n"
        "      luna_private_ref_drop_probe(51);\n"
        "      let forwarded = move error.first;\n"
        "    } else {\n"
        "      luna_private_ref_drop_probe(53);\n"
        "      let forwarded = move error.first;\n"
        "    }\n"
        "    return error.second.marker;\n"
        "  }\n"
        "}\n"
        "struct SourceEarlyContinueError { first: SourceError; second: SourceError; }\n"
        "impl From<SourceEarlyContinueError> for i32 {\n"
        "  fn from(affine error: SourceEarlyContinueError) -> i32 {\n"
        "    if error.first.marker > 0 {\n"
        "      luna_private_ref_drop_probe(51);\n"
        "      let forwarded = move error.first;\n"
        "      return forwarded.marker;\n"
        "    } else {\n"
        "      luna_private_ref_drop_probe(53);\n"
        "      let forwarded = move error.second;\n"
        "    }\n"
        "    return 0 - error.first.marker;\n"
        "  }\n"
        "}\n"
        "struct ReturnedResource { marker: i32; }\n"
        "impl Drop for ReturnedResource {\n"
        "  fn drop(resource: &mut ReturnedResource) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "impl From<SourceError> for ReturnedResource {\n"
        "  fn from(affine error: SourceError) -> ReturnedResource {\n"
        "    let returned = new ReturnedResource(error.marker + 12);\n"
        "    return move returned;\n"
        "  }\n"
        "}\n"
        "struct ReturnedFromSplit { marker: i32; inner: SourceError; }\n"
        "impl Drop for ReturnedFromSplit {\n"
        "  fn drop(resource: &mut ReturnedFromSplit) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "impl From<SourceSplitError> for ReturnedFromSplit {\n"
        "  fn from(affine error: SourceSplitError) -> ReturnedFromSplit {\n"
        "    if error.first.marker > 0 {\n"
        "      let carried = move error.first;\n"
        "      let returned = new ReturnedFromSplit(59, move carried);\n"
        "      return move returned;\n"
        "    }\n"
        "    let carried = move error.second;\n"
        "    let returned = new ReturnedFromSplit(59, move carried);\n"
        "    return move returned;\n"
        "  }\n"
        "}\n"
        "struct ReturnedPair { padding: i32; marker: i32; }\n"
        "impl Drop for ReturnedPair {\n"
        "  fn drop(resource: &mut ReturnedPair) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.padding + resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct ReturnedInner { marker: i32; }\n"
        "impl Drop for ReturnedInner {\n"
        "  fn drop(resource: &mut ReturnedInner) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct ReturnedOuter { marker: i32; inner: ReturnedInner; }\n"
        "impl Drop for ReturnedOuter {\n"
        "  fn drop(resource: &mut ReturnedOuter) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct DeepInner { marker: i32; }\n"
        "impl Drop for DeepInner {\n"
        "  fn drop(resource: &mut DeepInner) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct DeepMiddle { marker: i32; inner: DeepInner; }\n"
        "impl Drop for DeepMiddle {\n"
        "  fn drop(resource: &mut DeepMiddle) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct DeepOuter { marker: i32; middle: DeepMiddle; }\n"
        "impl Drop for DeepOuter {\n"
        "  fn drop(resource: &mut DeepOuter) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct BranchLeft { marker: i32; }\n"
        "impl Drop for BranchLeft {\n"
        "  fn drop(resource: &mut BranchLeft) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct BranchRight { marker: i32; }\n"
        "impl Drop for BranchRight {\n"
        "  fn drop(resource: &mut BranchRight) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct BranchOuter { marker: i32; left: BranchLeft; right: BranchRight; }\n"
        "impl Drop for BranchOuter {\n"
        "  fn drop(resource: &mut BranchOuter) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "struct BranchTooDeep { marker: i32; left: DeepMiddle; right: BranchRight; }\n"
        "impl Drop for BranchTooDeep {\n"
        "  fn drop(resource: &mut BranchTooDeep) -> unit {\n"
        "    luna_private_ref_drop_probe(resource.marker);\n"
        "    resource.marker = 0;\n"
        "  }\n"
        "}\n"
        "export slot checkpoint(value: i32);\n"
        "runtime fn try_apply(selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let input = Err::<i32, i32>(7);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_ok(selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let input = Ok::<i32, i32>(9);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_outlined_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      checkpoint(1) { return Err::<i32, i32>(7); }\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_after_slot("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let input = Err::<i32, i32>(7);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      checkpoint(1) {}\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_local_cleanup("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let input = Err::<i32, i32>(7);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let resource = new ApplyLocal(31);\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_from("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let error = new SourceError(47);\n"
        "  let input = Err::<i32, SourceError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_nested_from("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let inner = new SourceError(47);\n"
        "  let error = new SourceNestedError(43, move inner);\n"
        "  let input = Err::<i32, SourceNestedError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_nested_from_else("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let inner = new SourceError(47);\n"
        "  let error = new SourceNestedError(-43, move inner);\n"
        "  let input = Err::<i32, SourceNestedError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_split_from("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let first = new SourceError(43);\n"
        "  let second = new SourceError(47);\n"
        "  let error = new SourceSplitError(move first, move second);\n"
        "  let input = Err::<i32, SourceSplitError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_split_from_else("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let first = new SourceError(-43);\n"
        "  let second = new SourceError(47);\n"
        "  let error = new SourceSplitError(move first, move second);\n"
        "  let input = Err::<i32, SourceSplitError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_merge_from("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let first = new SourceError(43);\n"
        "  let second = new SourceError(47);\n"
        "  let error = new SourceMergeError(move first, move second);\n"
        "  let input = Err::<i32, SourceMergeError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_merge_from_else("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let first = new SourceError(-43);\n"
        "  let second = new SourceError(47);\n"
        "  let error = new SourceMergeError(move first, move second);\n"
        "  let input = Err::<i32, SourceMergeError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_early_continue_from("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let first = new SourceError(43);\n"
        "  let second = new SourceError(47);\n"
        "  let error = new SourceEarlyContinueError(move first, move second);\n"
        "  let input = Err::<i32, SourceEarlyContinueError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_early_continue_from_else("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, i32> {\n"
        "  let first = new SourceError(-43);\n"
        "  let second = new SourceError(47);\n"
        "  let error = new SourceEarlyContinueError(move first, move second);\n"
        "  let input = Err::<i32, SourceEarlyContinueError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_resource_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, ReturnedResource> {\n"
        "  let resource = new ReturnedResource(59);\n"
        "  let input = Err::<i32, ReturnedResource>(move resource);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_from_resource_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, ReturnedResource> {\n"
        "  let source = new SourceError(47);\n"
        "  let input = Err::<i32, SourceError>(move source);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_split_owned_from("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, ReturnedFromSplit> {\n"
        "  let first = new SourceError(43);\n"
        "  let second = new SourceError(47);\n"
        "  let error = new SourceSplitError(move first, move second);\n"
        "  let input = Err::<i32, SourceSplitError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_split_owned_from_else("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, ReturnedFromSplit> {\n"
        "  let first = new SourceError(-43);\n"
        "  let second = new SourceError(47);\n"
        "  let error = new SourceSplitError(move first, move second);\n"
        "  let input = Err::<i32, SourceSplitError>(move error);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_ok_resource_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<ReturnedResource, i32> {\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      checkpoint(1) {}\n"
        "      let resource = new ReturnedResource(67);\n"
        "      return Ok::<ReturnedResource, i32>(move resource);\n"
        "    }\n"
        "  }\n"
        "  return Err::<ReturnedResource, i32>(0);\n"
        "}\n"
        "runtime fn try_apply_pair_resource_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, ReturnedPair> {\n"
        "  let resource = new ReturnedPair(5, 61);\n"
        "  let input = Err::<i32, ReturnedPair>(move resource);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_ok_resource_scalar_err("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<ReturnedResource, i32> {\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      checkpoint(1) {}\n"
        "      return Err::<ReturnedResource, i32>(13);\n"
        "    }\n"
        "  }\n"
        "  return Err::<ReturnedResource, i32>(0);\n"
        "}\n"
        "runtime fn try_apply_err_resource_scalar_ok("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, ReturnedPair> {\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      checkpoint(1) {}\n"
        "      return Ok::<i32, ReturnedPair>(14);\n"
        "    }\n"
        "  }\n"
        "  return Ok::<i32, ReturnedPair>(0);\n"
        "}\n"
        "runtime fn try_apply_nested_resource_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, ReturnedOuter> {\n"
        "  let inner = new ReturnedInner(73);\n"
        "  let outer = new ReturnedOuter(71, move inner);\n"
        "  let input = Err::<i32, ReturnedOuter>(move outer);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_deep_resource_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, DeepOuter> {\n"
        "  let inner = new DeepInner(83);\n"
        "  let middle = new DeepMiddle(81, move inner);\n"
        "  let outer = new DeepOuter(79, move middle);\n"
        "  let input = Err::<i32, DeepOuter>(move outer);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_branch_resource_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, BranchOuter> {\n"
        "  let left = new BranchLeft(91);\n"
        "  let right = new BranchRight(93);\n"
        "  let outer = new BranchOuter(89, move left, move right);\n"
        "  let input = Err::<i32, BranchOuter>(move outer);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n"
        "runtime fn try_apply_oversized_branch_return("
            "selected: RuntimeFragmentRef<checkpoint>) "
            "-> Result<i32, BranchTooDeep> {\n"
        "  let inner = new DeepInner(83);\n"
        "  let left = new DeepMiddle(81, move inner);\n"
        "  let right = new BranchRight(93);\n"
        "  let outer = new BranchTooDeep(89, move left, move right);\n"
        "  let input = Err::<i32, BranchTooDeep>(move outer);\n"
        "  apply selected {\n"
        "    apply selected {\n"
        "      let value = input?;\n"
        "      checkpoint(value) {}\n"
        "    }\n"
        "  }\n"
        "  return Ok(0);\n"
        "}\n",
        "<canonical-nested-ref-try-cleanup>");
    if (!tryApplySnapshot.success())
        return fail("frontend rejected a Result propagation inside Ref apply");
    moon::LunaLowerer tryApplyLowerer;
    auto tryApplyModule = tryApplyLowerer.lower(
        *tryApplySnapshot.program(), *tryApplySnapshot.symbolTable());
    moon::Sealer tryApplySealer;
    if (!tryApplyModule || !tryApplyLowerer.errors().empty() ||
        !tryApplySealer.sealFunctionBodies(*tryApplyModule)) {
        for (const auto& diagnostic : tryApplySealer.errors())
            std::cerr << diagnostic << '\n';
        return fail("Ref apply Result propagation did not seal");
    }
    moon::FunctionDecl* tryApply = nullptr;
    moon::FunctionDecl* tryApplyOk = nullptr;
    moon::FunctionDecl* tryApplyOutlinedReturn = nullptr;
    moon::FunctionDecl* tryApplyAfterSlot = nullptr;
    moon::FunctionDecl* tryApplyLocalCleanup = nullptr;
    moon::FunctionDecl* tryApplyFrom = nullptr;
    moon::FunctionDecl* tryApplyNestedFrom = nullptr;
    moon::FunctionDecl* tryApplyNestedFromElse = nullptr;
    moon::FunctionDecl* tryApplySplitFrom = nullptr;
    moon::FunctionDecl* tryApplySplitFromElse = nullptr;
    moon::FunctionDecl* tryApplyMergeFrom = nullptr;
    moon::FunctionDecl* tryApplyMergeFromElse = nullptr;
    moon::FunctionDecl* tryApplyEarlyContinueFrom = nullptr;
    moon::FunctionDecl* tryApplyEarlyContinueFromElse = nullptr;
    moon::FunctionDecl* tryApplyResourceReturn = nullptr;
    moon::FunctionDecl* tryApplyFromResourceReturn = nullptr;
    moon::FunctionDecl* tryApplySplitOwnedFrom = nullptr;
    moon::FunctionDecl* tryApplySplitOwnedFromElse = nullptr;
    moon::FunctionDecl* tryApplyOkResourceReturn = nullptr;
    moon::FunctionDecl* tryApplyPairResourceReturn = nullptr;
    moon::FunctionDecl* tryApplyOkResourceScalarErr = nullptr;
    moon::FunctionDecl* tryApplyErrResourceScalarOk = nullptr;
    moon::FunctionDecl* tryApplyNestedResourceReturn = nullptr;
    moon::FunctionDecl* tryApplyDeepResourceReturn = nullptr;
    moon::FunctionDecl* tryApplyBranchResourceReturn = nullptr;
    moon::FunctionDecl* tryApplyOversizedBranchReturn = nullptr;
    for (auto& declaration : tryApplyModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function) {
            if (function->name == "try_apply") tryApply = function;
            if (function->name == "try_apply_ok") tryApplyOk = function;
            if (function->name == "try_apply_outlined_return")
                tryApplyOutlinedReturn = function;
            if (function->name == "try_apply_after_slot")
                tryApplyAfterSlot = function;
            if (function->name == "try_apply_local_cleanup")
                tryApplyLocalCleanup = function;
            if (function->name == "try_apply_from")
                tryApplyFrom = function;
            if (function->name == "try_apply_nested_from")
                tryApplyNestedFrom = function;
            if (function->name == "try_apply_nested_from_else")
                tryApplyNestedFromElse = function;
            if (function->name == "try_apply_split_from")
                tryApplySplitFrom = function;
            if (function->name == "try_apply_split_from_else")
                tryApplySplitFromElse = function;
            if (function->name == "try_apply_merge_from")
                tryApplyMergeFrom = function;
            if (function->name == "try_apply_merge_from_else")
                tryApplyMergeFromElse = function;
            if (function->name == "try_apply_early_continue_from")
                tryApplyEarlyContinueFrom = function;
            if (function->name == "try_apply_early_continue_from_else")
                tryApplyEarlyContinueFromElse = function;
            if (function->name == "try_apply_resource_return")
                tryApplyResourceReturn = function;
            if (function->name == "try_apply_from_resource_return")
                tryApplyFromResourceReturn = function;
            if (function->name == "try_apply_split_owned_from")
                tryApplySplitOwnedFrom = function;
            if (function->name == "try_apply_split_owned_from_else")
                tryApplySplitOwnedFromElse = function;
            if (function->name == "try_apply_ok_resource_return")
                tryApplyOkResourceReturn = function;
            if (function->name == "try_apply_pair_resource_return")
                tryApplyPairResourceReturn = function;
            if (function->name == "try_apply_ok_resource_scalar_err")
                tryApplyOkResourceScalarErr = function;
            if (function->name == "try_apply_err_resource_scalar_ok")
                tryApplyErrResourceScalarOk = function;
            if (function->name == "try_apply_nested_resource_return")
                tryApplyNestedResourceReturn = function;
            if (function->name == "try_apply_deep_resource_return")
                tryApplyDeepResourceReturn = function;
            if (function->name == "try_apply_branch_resource_return")
                tryApplyBranchResourceReturn = function;
            if (function->name == "try_apply_oversized_branch_return")
                tryApplyOversizedBranchReturn = function;
        }
    if (!tryApply || !tryApplyOk || !tryApplyOutlinedReturn ||
        !tryApplyAfterSlot || !tryApplyLocalCleanup || !tryApplyFrom ||
        !tryApplyNestedFrom || !tryApplyNestedFromElse ||
        !tryApplySplitFrom || !tryApplySplitFromElse ||
        !tryApplyMergeFrom || !tryApplyMergeFromElse ||
        !tryApplyEarlyContinueFrom || !tryApplyEarlyContinueFromElse ||
        !tryApplyResourceReturn || !tryApplyFromResourceReturn ||
        !tryApplySplitOwnedFrom || !tryApplySplitOwnedFromElse ||
        !tryApplyOkResourceReturn ||
        !tryApplyPairResourceReturn || !tryApplyOkResourceScalarErr ||
        !tryApplyErrResourceScalarOk || !tryApplyNestedResourceReturn ||
        !tryApplyDeepResourceReturn || !tryApplyBranchResourceReturn ||
        !tryApplyOversizedBranchReturn ||
        !tryApply->controlFlow ||
        tryApply->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApply->controlFlow, *tryApplyModule))
        return fail("Ref apply Result propagation failed CFG verification");
    const auto tryFlow = moon::planRuntimeRefApplyFlow(
        *tryApply->controlFlow, refApplyFlowError);
    const auto expectedTryExits = std::vector<moon::RegionId>{
        tryApply->controlFlow->runtimeRefApplies[1].region,
        tryApply->controlFlow->runtimeRefApplies[0].region};
    size_t trySwitches = 0;
    size_t propagatedErrors = 0;
    moon::BasicBlock* trySwitchBlock = nullptr;
    for (auto& block : tryApply->controlFlow->blocks) {
        if (block.terminator.kind == moon::TerminatorKind::Switch) {
            ++trySwitches;
            trySwitchBlock = &block;
            if (!tryFlow ||
                tryFlow->activeByBlock[block.id.value] !=
                    std::vector<moon::RegionId>{
                        expectedTryExits[1], expectedTryExits[0]})
                return fail("Ref apply Result switch lost its active contexts");
            for (const auto& edge : tryFlow->edges)
                if (edge.source == block.id &&
                    (!edge.enters.empty() || !edge.exits.empty()))
                    return fail("Ref apply Result switch crossed a context boundary");
        }
        if (block.terminator.kind == moon::TerminatorKind::Return) {
            const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
                block.terminator.operand.get());
            if (!result || result->isOk) continue;
            ++propagatedErrors;
            if (!tryFlow || !std::any_of(
                    tryFlow->terminals.begin(), tryFlow->terminals.end(),
                    [&](const auto& terminal) {
                        return terminal.block == block.id &&
                            terminal.exits == expectedTryExits;
                    }))
                return fail("Ref apply '?' return lost ordered context exits");
        }
    }
    if (!tryFlow || trySwitches != 1 || propagatedErrors != 1)
        return fail("Ref apply '?' did not retain one Switch and Err return");
    if (!trySwitchBlock || !moon::isExhaustiveResultDefault(
            *tryApply->controlFlow, *tryApplyModule, *tryFlow,
            trySwitchBlock->terminator.primary.target))
        return fail("Ref apply '?' default is not an exhaustive Result arm");
    const auto originalTryTag = trySwitchBlock->terminator.cases[1].tag;
    trySwitchBlock->terminator.cases[1].tag =
        trySwitchBlock->terminator.cases[0].tag;
    const bool duplicateTagAccepted = moon::isExhaustiveResultDefault(
        *tryApply->controlFlow, *tryApplyModule, *tryFlow,
        trySwitchBlock->terminator.primary.target);
    trySwitchBlock->terminator.cases[1].tag = originalTryTag;
    if (duplicateTagAccepted)
        return fail("non-exhaustive Result default bypassed private cleanup gate");
    if (!tryApplyOk->controlFlow ||
        tryApplyOk->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyOk->controlFlow, *tryApplyModule))
        return fail("successful Ref apply Result propagation failed CFG verification");
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApply, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 7})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply Err propagation failed private JIT execution");
    }
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyOk, privateRefJitError,
            1, false, std::nullopt, false, false, {9}, {},
            std::pair<bool, int32_t>{true, 0})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply Ok propagation failed private JIT execution");
    }
    if (!tryApplyOutlinedReturn->controlFlow ||
        tryApplyOutlinedReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(
            *tryApplyOutlinedReturn->controlFlow, *tryApplyModule) ||
        !exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyOutlinedReturn,
            privateRefJitError, 1, true, 0, false, false, {1}, {},
            std::pair<bool, int32_t>{false, 7})) {
        std::cerr << privateRefJitError << '\n';
        return fail("outlined Ref apply Result return failed private JIT execution");
    }
    if (!tryApplyAfterSlot->controlFlow ||
        tryApplyAfterSlot->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(
            *tryApplyAfterSlot->controlFlow, *tryApplyModule) ||
        !exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyAfterSlot,
            privateRefJitError, 1, false, 1, false, false, {1}, {},
            std::pair<bool, int32_t>{false, 7})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' after Slot dispatch failed private JIT execution");
    }
    if (!tryApplyLocalCleanup->controlFlow ||
        tryApplyLocalCleanup->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(
            *tryApplyLocalCleanup->controlFlow, *tryApplyModule))
        return fail("Ref apply '?' local resource did not verify");
    bool hasLocalErrDrop = false;
    for (const auto& block : tryApplyLocalCleanup->controlFlow->blocks) {
        const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
            block.terminator.operand.get());
        if (block.terminator.kind != moon::TerminatorKind::Return ||
            !result || result->isOk)
            continue;
        for (const auto cleanupId : block.terminator.exitCleanups) {
            const auto* cleanup =
                tryApplyLocalCleanup->controlFlow->findCleanup(cleanupId);
            const auto* scope = cleanup
                ? tryApplyLocalCleanup->controlFlow->findScope(cleanup->scope)
                : nullptr;
            if (!cleanup || !scope || cleanup->action !=
                    luna::ownership::CleanupAction::Drop)
                continue;
            for (auto region = scope->region; !region.empty();) {
                if (region ==
                    tryApplyLocalCleanup->controlFlow->runtimeRefApplies[1]
                        .region) {
                    hasLocalErrDrop = true;
                    break;
                }
                const auto* record =
                    tryApplyLocalCleanup->controlFlow->findRegion(region);
                if (!record) break;
                region = record->parent;
            }
        }
    }
    if (!hasLocalErrDrop)
        return fail("Ref apply '?' Err return lost local custom Drop cleanup");
    const auto dropProbeBefore = privateRefJitDropProbeCalls;
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyLocalCleanup,
            privateRefJitError, 0, false, 0, false, false, {}, {},
            std::pair<bool, int32_t>{false, 7})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' local cleanup failed private JIT execution");
    }
    if (privateRefJitDropProbeCalls != dropProbeBefore + 2 ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' did not run exact local Drop once per call");
    if (!tryApplyFrom->controlFlow ||
        tryApplyFrom->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyFrom->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' From conversion did not verify");
    bool hasFrozenFromReturn = false;
    for (const auto& block : tryApplyFrom->controlFlow->blocks) {
        if (block.terminator.kind != moon::TerminatorKind::Return)
            continue;
        const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
            block.terminator.operand.get());
        const auto* conversion = result && !result->isOk
            ? dynamic_cast<const moon::CallExpr*>(result->payload.get())
            : nullptr;
        const auto* frozen = conversion
            ? tryApplyModule->findDeclaration(conversion->calleeRef)
            : nullptr;
        if (frozen && frozen->sourceName == "from")
            hasFrozenFromReturn = true;
    }
    if (!hasFrozenFromReturn)
        return fail("Ref apply '?' Err return lost its frozen From witness");
    const auto conversionDropBefore = privateRefJitConversionDropProbeCalls;
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyFrom, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 47})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' From conversion failed private JIT execution");
    }
    if (privateRefJitConversionDropProbeCalls != conversionDropBefore + 2 ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' From did not consume and Drop its source once per call");
    if (!tryApplyNestedFrom->controlFlow ||
        tryApplyNestedFrom->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyNestedFrom->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' nested From conversion did not verify");
    privateRefJitConversionDropOrder.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyNestedFrom, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 43})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' nested From conversion failed private JIT execution");
    }
    if (privateRefJitConversionDropOrder !=
            std::vector<int32_t>{43, 47, 43, 47} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' nested From lost ordered source cleanup");
    if (!tryApplyNestedFromElse->controlFlow ||
        tryApplyNestedFromElse->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyNestedFromElse->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' nested From else branch did not verify");
    privateRefJitConversionDropOrder.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyNestedFromElse, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 43})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' nested From else branch failed private JIT execution");
    }
    if (privateRefJitConversionDropOrder !=
            std::vector<int32_t>{-43, 47, -43, 47} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' nested From else branch lost ordered cleanup");
    if (!tryApplySplitFrom->controlFlow ||
        tryApplySplitFrom->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplySplitFrom->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' split-field From conversion did not verify");
    privateRefJitConversionDropOrder.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplySplitFrom, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 43})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' split-field From conversion failed private JIT execution");
    }
    if (privateRefJitConversionDropOrder !=
            std::vector<int32_t>{43, 47, 43, 47} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' split-field From lost ordered field cleanup");
    if (!tryApplySplitFromElse->controlFlow ||
        tryApplySplitFromElse->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplySplitFromElse->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' split-field From else branch did not verify");
    privateRefJitConversionDropOrder.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplySplitFromElse, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 47})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' split-field From else branch failed private JIT execution");
    }
    if (privateRefJitConversionDropOrder !=
            std::vector<int32_t>{47, -43, 47, -43} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' split-field From else branch lost ordered cleanup");
    if (!tryApplyMergeFrom->controlFlow ||
        tryApplyMergeFrom->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyMergeFrom->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' merged-field From conversion did not verify");
    privateRefJitConversionDropOrder.clear();
    privateRefJitFromBranchMarkers.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyMergeFrom, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 47})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' merged-field From conversion failed private JIT execution");
    }
    if (privateRefJitConversionDropOrder !=
            std::vector<int32_t>{43, 47, 43, 47} ||
        privateRefJitFromBranchMarkers != std::vector<int32_t>{51, 51} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' merged-field From lost ordered cleanup");
    if (!tryApplyMergeFromElse->controlFlow ||
        tryApplyMergeFromElse->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyMergeFromElse->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' merged-field From else branch did not verify");
    privateRefJitConversionDropOrder.clear();
    privateRefJitFromBranchMarkers.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyMergeFromElse, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 47})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' merged-field From else branch failed private JIT execution");
    }
    if (privateRefJitConversionDropOrder !=
            std::vector<int32_t>{-43, 47, -43, 47} ||
        privateRefJitFromBranchMarkers != std::vector<int32_t>{53, 53} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' merged-field From else branch lost ordered cleanup");
    if (!tryApplyEarlyContinueFrom->controlFlow ||
        tryApplyEarlyContinueFrom->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyEarlyContinueFrom->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' early/continuing From branch did not verify");
    privateRefJitConversionDropOrder.clear();
    privateRefJitFromBranchMarkers.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyEarlyContinueFrom, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 43})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' early-return From branch failed private JIT execution");
    }
    if (privateRefJitConversionDropOrder !=
            std::vector<int32_t>{43, 47, 43, 47} ||
        privateRefJitFromBranchMarkers != std::vector<int32_t>{51, 51} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' early-return From branch lost ordered cleanup");
    if (!tryApplyEarlyContinueFromElse->controlFlow ||
        tryApplyEarlyContinueFromElse->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyEarlyContinueFromElse->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' continuing From branch did not verify");
    privateRefJitConversionDropOrder.clear();
    privateRefJitFromBranchMarkers.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyEarlyContinueFromElse, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 43})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' continuing From branch failed private JIT execution");
    }
    if (privateRefJitConversionDropOrder !=
            std::vector<int32_t>{47, -43, 47, -43} ||
        privateRefJitFromBranchMarkers != std::vector<int32_t>{53, 53} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' continuing From branch lost ordered cleanup");
    if (!tryApplyResourceReturn->controlFlow ||
        tryApplyResourceReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyResourceReturn->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' resource return did not verify");
    const auto resourceFlow = moon::planRuntimeRefApplyFlow(
        *tryApplyResourceReturn->controlFlow, refApplyFlowError);
    const auto resourceExits = std::vector<moon::RegionId>{
        tryApplyResourceReturn->controlFlow->runtimeRefApplies[1].region,
        tryApplyResourceReturn->controlFlow->runtimeRefApplies[0].region};
    const auto* resourceResultType = tryApplyModule->findType(
        tryApplyResourceReturn->returnType);
    if (!resourceResultType ||
        resourceResultType->kind != TypeKind::Result ||
        resourceResultType->typeArgumentIds.size() != 2)
        return fail("resource Err return lost its frozen Result type");
    luna::codegen::NativeOwnedResultSourceFacts ownedSourceFacts;
    std::string ownedSourceError;
    const auto* ownedErrorType = tryApplyModule->findType(
        resourceResultType->typeArgumentIds[1]);
    const auto refTarget = tryApplyModule->resolveRuntimeFragmentRefTarget(
        tryApplyResourceReturn->params.front().type);
    if (!ownedErrorType || !refTarget ||
        !luna::codegen::deriveNativeOwnedResultSourceFacts(
            *tryApplyModule, *tryApplyResourceReturn,
            ownedSourceFacts, ownedSourceError)) {
        std::cerr << ownedSourceError << '\n';
        return fail("Native v3 candidate could not derive frozen Ref/Result facts");
    }
    if (ownedSourceFacts.functionSymbolId !=
            tryApplyResourceReturn->symbolId.value ||
        ownedSourceFacts.functionContractId !=
            tryApplyResourceReturn->contractId.value ||
        ownedSourceFacts.sourceLinkageName !=
            tryApplyResourceReturn->generatedSymbolName ||
        ownedSourceFacts.refSlotSymbolId != refTarget->symbol.value ||
        ownedSourceFacts.refSlotContractId != refTarget->contract.value ||
        ownedSourceFacts.resultTypeId != resourceResultType->id.value ||
        ownedSourceFacts.errorTypeId != ownedErrorType->id.value ||
        ownedSourceFacts.errorAbiLayoutId !=
            ownedErrorType->abiLayoutId.value ||
        ownedSourceFacts.errorValueSize != ownedErrorType->valueSize ||
        ownedSourceFacts.errorValueAlignment !=
            ownedErrorType->valueAlignment ||
        ownedSourceFacts.errorDropSymbolId !=
            ownedErrorType->dropGlue.symbol.value ||
        ownedSourceFacts.errorDropContractId !=
            ownedErrorType->dropGlue.contract.value)
        return fail("Native v3 source facts lost frozen identities or layout");
    auto mutableOwnedError = std::find_if(
        tryApplyModule->typeTable.begin(), tryApplyModule->typeTable.end(),
        [&](const moon::TypeRecord& type) {
            return type.id == ownedErrorType->id;
        });
    if (mutableOwnedError == tryApplyModule->typeTable.end())
        return fail("Native v3 candidate lost its mutable test copy");
    const auto frozenLayoutId = mutableOwnedError->abiLayoutId;
    mutableOwnedError->abiLayoutId.value = "forged-layout";
    const bool acceptedForgedLayout =
        luna::codegen::deriveNativeOwnedResultSourceFacts(
            *tryApplyModule, *tryApplyResourceReturn,
            ownedSourceFacts, ownedSourceError);
    mutableOwnedError->abiLayoutId = frozenLayoutId;
    if (acceptedForgedLayout)
        return fail("Native v3 candidate accepted a forged owner layout");
    const auto frozenDropGlue = mutableOwnedError->dropGlue;
    mutableOwnedError->dropGlue = {};
    const bool acceptedMissingDrop =
        luna::codegen::deriveNativeOwnedResultSourceFacts(
            *tryApplyModule, *tryApplyResourceReturn,
            ownedSourceFacts, ownedSourceError);
    mutableOwnedError->dropGlue = frozenDropGlue;
    if (acceptedMissingDrop)
        return fail("Native v3 candidate accepted missing frozen Drop glue");
    const auto frozenApplySlot =
        tryApplyResourceReturn->controlFlow->runtimeRefApplies.front().slot;
    tryApplyResourceReturn->controlFlow->runtimeRefApplies.front().slot = {};
    const bool acceptedChangedTarget =
        luna::codegen::deriveNativeOwnedResultSourceFacts(
            *tryApplyModule, *tryApplyResourceReturn,
            ownedSourceFacts, ownedSourceError);
    tryApplyResourceReturn->controlFlow->runtimeRefApplies.front().slot =
        frozenApplySlot;
    if (acceptedChangedTarget)
        return fail("Native v3 candidate accepted a changed Ref target");
    if (luna::codegen::deriveNativeOwnedResultSourceFacts(
            *tryApplyModule, *tryApplyFrom,
            ownedSourceFacts, ownedSourceError))
        return fail("Native v3 candidate accepted a scalar Result error");
    bool hasResourceErrReturn = false;
    for (const auto& block : tryApplyResourceReturn->controlFlow->blocks) {
        if (block.terminator.kind != moon::TerminatorKind::Return)
            continue;
        const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
            block.terminator.operand.get());
        if (!result || result->isOk || !result->payload)
            continue;
        if (result->payload->type !=
                resourceResultType->typeArgumentIds[1] ||
            !dynamic_cast<const moon::MoveExpr*>(result->payload.get()))
            return fail("resource Err return did not transfer its owned payload");
        if (!resourceFlow || !std::any_of(
                resourceFlow->terminals.begin(),
                resourceFlow->terminals.end(),
                [&](const auto& terminal) {
                    return terminal.block == block.id &&
                        terminal.exits == resourceExits;
                }))
            return fail("resource Err return lost ordered context exits");
        hasResourceErrReturn = true;
    }
    if (!hasResourceErrReturn)
        return fail("Ref apply '?' resource return lost its Err payload");
    const auto returnedDropBefore = privateRefJitReturnedDropProbeCalls;
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyResourceReturn,
            privateRefJitError, 0, false, std::nullopt, false, false,
            {}, {}, std::pair<bool, int32_t>{false, 59})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' resource return failed private JIT execution");
    }
    if (privateRefJitReturnedDropProbeCalls != returnedDropBefore + 2 ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' returned resource was not dropped once per call");
    if (!tryApplyFromResourceReturn->controlFlow ||
        tryApplyFromResourceReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyFromResourceReturn->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply '?' From resource return did not verify");
    const auto* fromResourceResultType = tryApplyModule->findType(
        tryApplyFromResourceReturn->returnType);
    if (!fromResourceResultType ||
        fromResourceResultType->kind != TypeKind::Result ||
        fromResourceResultType->typeArgumentIds.size() != 2)
        return fail("Ref apply '?' From resource return lost its Result type");
    bool hasFrozenOwnedFromReturn = false;
    for (const auto& block : tryApplyFromResourceReturn->controlFlow->blocks) {
        if (block.terminator.kind != moon::TerminatorKind::Return)
            continue;
        const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
            block.terminator.operand.get());
        const auto* conversion = result && !result->isOk
            ? dynamic_cast<const moon::CallExpr*>(result->payload.get())
            : nullptr;
        const auto* frozen = conversion
            ? tryApplyModule->findDeclaration(conversion->calleeRef)
            : nullptr;
        if (frozen && frozen->sourceName == "from" &&
            conversion->type ==
                fromResourceResultType->typeArgumentIds[1])
            hasFrozenOwnedFromReturn = true;
    }
    if (!hasFrozenOwnedFromReturn)
        return fail("Ref apply '?' owned Err lost its frozen From witness");
    const auto fromResourceSourceDrops = privateRefJitConversionDropProbeCalls;
    const auto fromResourceReturnedDrops = privateRefJitReturnedDropProbeCalls;
    privateRefJitFromResourceDropOrder.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyFromResourceReturn, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 59})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' From resource return failed private JIT execution");
    }
    if (privateRefJitConversionDropProbeCalls != fromResourceSourceDrops + 2 ||
        privateRefJitReturnedDropProbeCalls != fromResourceReturnedDrops + 2 ||
        privateRefJitFromResourceDropOrder !=
            std::vector<int32_t>{47, 59, 47, 59} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' From resource return lost source/owner Drop order");
    for (auto* function : {tryApplySplitOwnedFrom,
                           tryApplySplitOwnedFromElse}) {
        if (!function->controlFlow ||
            function->controlFlow->runtimeRefApplies.size() != 2 ||
            !refApplyCfgVerifier.verify(*function->controlFlow,
                                        *tryApplyModule))
            return fail("Ref apply '?' split-owned From return did not verify");
        const auto* resultType = tryApplyModule->findType(function->returnType);
        if (!resultType || resultType->kind != TypeKind::Result ||
            resultType->typeArgumentIds.size() != 2)
            return fail("Ref apply '?' split-owned From return lost its Result type");
        bool hasFrozenFromReturn = false;
        for (const auto& block : function->controlFlow->blocks) {
            if (block.terminator.kind != moon::TerminatorKind::Return)
                continue;
            const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
                block.terminator.operand.get());
            const auto* conversion = result && !result->isOk
                ? dynamic_cast<const moon::CallExpr*>(result->payload.get())
                : nullptr;
            const auto* frozen = conversion
                ? tryApplyModule->findDeclaration(conversion->calleeRef)
                : nullptr;
            if (frozen && frozen->sourceName == "from" &&
                conversion->type == resultType->typeArgumentIds[1])
                hasFrozenFromReturn = true;
        }
        if (!hasFrozenFromReturn)
            return fail("Ref apply '?' split-owned Err lost its frozen From witness");
    }
    const auto splitOwnedRemainingDrops = privateRefJitConversionDropProbeCalls;
    const auto splitOwnedReturnedDrops = privateRefJitReturnedDropProbeCalls;
    const auto splitOwnedAllDrops = privateRefJitAllDropProbeCalls;
    privateRefJitFromResourceDropOrder.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplySplitOwnedFrom, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 59})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' split-owned From return failed private JIT execution");
    }
    if (privateRefJitConversionDropProbeCalls != splitOwnedRemainingDrops + 2 ||
        privateRefJitReturnedDropProbeCalls != splitOwnedReturnedDrops + 2 ||
        privateRefJitAllDropProbeCalls != splitOwnedAllDrops + 6 ||
        privateRefJitFromResourceDropOrder !=
            std::vector<int32_t>{47, 59, 43, 47, 59, 43} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' split-owned From return lost field/owner Drop order");
    const auto splitOwnedElseAllDrops = privateRefJitAllDropProbeCalls;
    const auto splitOwnedElseReturnedDrops = privateRefJitReturnedDropProbeCalls;
    privateRefJitFromResourceDropOrder.clear();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplySplitOwnedFromElse, privateRefJitError,
            0, false, std::nullopt, false, false, {}, {},
            std::pair<bool, int32_t>{false, 59})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply '?' split-owned From else failed private JIT execution");
    }
    if (privateRefJitAllDropProbeCalls != splitOwnedElseAllDrops + 6 ||
        privateRefJitReturnedDropProbeCalls != splitOwnedElseReturnedDrops + 2 ||
        privateRefJitFromResourceDropOrder !=
            std::vector<int32_t>{-43, 59, 47, -43, 59, 47} ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply '?' split-owned From else lost field/owner Drop order");
    for (const auto& transferCase : {
             std::pair{tryApplySplitOwnedFrom,
                       std::vector<int32_t>{47, 59, 43}},
             std::pair{tryApplySplitOwnedFromElse,
                       std::vector<int32_t>{-43, 59, 47}}}) {
        const auto allDropsBefore = privateRefJitAllDropProbeCalls;
        const auto returnedDropsBefore = privateRefJitReturnedDropProbeCalls;
        privateRefJitFromResourceDropOrder.clear();
        privateRefJitDropProbeValid = true;
        if (!exercisePrivateRefApplyJit(
                *tryApplyModule, *transferCase.first, privateRefJitError,
                0, false, std::nullopt, false, false, {}, {},
                std::pair<bool, int32_t>{false, 59}, 2, true, true,
                false, nullptr, 1)) {
            std::cerr << privateRefJitError << '\n';
            return fail("Ref apply split-owned From host transfer failed private JIT execution");
        }
        std::vector<int32_t> expectedOrder;
        for (unsigned call = 0; call < 4; ++call)
            expectedOrder.insert(expectedOrder.end(),
                transferCase.second.begin(), transferCase.second.end());
        if (privateRefJitAllDropProbeCalls != allDropsBefore + 12 ||
            privateRefJitReturnedDropProbeCalls != returnedDropsBefore + 4 ||
            privateRefJitFromResourceDropOrder != expectedOrder ||
            !privateRefJitDropProbeValid)
            return fail("Ref apply split-owned From host transfer lost Drop order");
    }
    if (!tryApplyOkResourceReturn->controlFlow ||
        tryApplyOkResourceReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyOkResourceReturn->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply Ok resource return did not verify");
    const auto returnedOkDropBefore = privateRefJitReturnedOkDropProbeCalls;
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyOkResourceReturn,
            privateRefJitError, 1, false, 1, false, false,
            {1}, {}, std::pair<bool, int32_t>{true, 67})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply Ok resource return failed private JIT execution");
    }
    if (privateRefJitReturnedOkDropProbeCalls != returnedOkDropBefore + 2 ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply Ok returned resource was not dropped once per call");
    if (!tryApplyPairResourceReturn->controlFlow ||
        tryApplyPairResourceReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyPairResourceReturn->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply pair resource return did not verify");
    const auto returnedPairDropBefore = privateRefJitReturnedPairDropProbeCalls;
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyPairResourceReturn,
            privateRefJitError, 0, false, std::nullopt, false, false,
            {}, {}, std::pair<bool, int32_t>{false, 61})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply pair resource return failed private JIT execution");
    }
    if (privateRefJitReturnedPairDropProbeCalls != returnedPairDropBefore + 2 ||
        !privateRefJitDropProbeValid)
        return fail("Ref apply pair returned resource was not dropped once per call");
    const auto scalarOkDropBefore = privateRefJitReturnedPairDropProbeCalls;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyErrResourceScalarOk,
            privateRefJitError, 1, false, 1, false, false,
            {1}, {}, std::pair<bool, int32_t>{true, 14})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply scalar Ok in resource Result failed private JIT execution");
    }
    if (privateRefJitReturnedPairDropProbeCalls != scalarOkDropBefore)
        return fail("Ref apply scalar Ok incorrectly dropped an Err resource");
    const auto scalarErrDropBefore = privateRefJitReturnedOkDropProbeCalls;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyOkResourceScalarErr,
            privateRefJitError, 1, false, 1, false, false,
            {1}, {}, std::pair<bool, int32_t>{false, 13})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply scalar Err in resource Result failed private JIT execution");
    }
    if (privateRefJitReturnedOkDropProbeCalls != scalarErrDropBefore)
        return fail("Ref apply scalar Err incorrectly dropped an Ok resource");
    if (!tryApplyNestedResourceReturn->controlFlow ||
        tryApplyNestedResourceReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyNestedResourceReturn->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply nested resource return did not verify");
    const auto nestedDropBefore = privateRefJitNestedDropOrder.size();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyNestedResourceReturn,
            privateRefJitError, 0, false, std::nullopt, false, false,
            {}, {}, std::pair<bool, int32_t>{false, 71})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply nested resource return failed private JIT execution");
    }
    if (!privateRefJitDropProbeValid ||
        privateRefJitNestedDropOrder.size() != nestedDropBefore + 4 ||
        !std::equal(privateRefJitNestedDropOrder.begin() + nestedDropBefore,
                    privateRefJitNestedDropOrder.end(),
                    std::array<int32_t, 4>{71, 73, 71, 73}.begin()))
        return fail("Ref apply nested resource return lost outer-before-inner Drop order");
    if (!tryApplyDeepResourceReturn->controlFlow ||
        tryApplyDeepResourceReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyDeepResourceReturn->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply deep resource return did not verify");
    const auto deepDropBefore = privateRefJitNestedDropOrder.size();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyDeepResourceReturn,
            privateRefJitError, 0, false, std::nullopt, false, false,
            {}, {}, std::pair<bool, int32_t>{false, 79})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply deep resource return failed private JIT execution");
    }
    if (!privateRefJitDropProbeValid ||
        privateRefJitNestedDropOrder.size() != deepDropBefore + 6 ||
        !std::equal(privateRefJitNestedDropOrder.begin() + deepDropBefore,
                    privateRefJitNestedDropOrder.end(),
                    std::array<int32_t, 6>{79, 81, 83, 79, 81, 83}.begin()))
        return fail("Ref apply deep resource return lost ordered Drop chain");
    const auto transferredDeepDropBefore = privateRefJitNestedDropOrder.size();
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyDeepResourceReturn,
            privateRefJitError, 0, false, std::nullopt, false, false,
            {}, {}, std::pair<bool, int32_t>{false, 79}, 3, true, true)) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply deep resource host transfer failed private JIT execution");
    }
    if (!privateRefJitDropProbeValid ||
        privateRefJitNestedDropOrder.size() != transferredDeepDropBefore + 12 ||
        !std::equal(privateRefJitNestedDropOrder.begin() + transferredDeepDropBefore,
                    privateRefJitNestedDropOrder.end(),
                    std::array<int32_t, 12>{79, 81, 83, 79, 81, 83,
                                             79, 81, 83, 79, 81, 83}.begin()))
        return fail("Ref apply transferred resource lost ordered Drop chain");
    if (!tryApplyBranchResourceReturn->controlFlow ||
        tryApplyBranchResourceReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyBranchResourceReturn->controlFlow,
                                    *tryApplyModule))
        return fail("Ref apply branching resource return did not verify");
    const auto branchDropBefore = privateRefJitNestedDropOrder.size();
    privateRefJitDropProbeValid = true;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyBranchResourceReturn,
            privateRefJitError, 0, false, std::nullopt, false, false,
            {}, {}, std::pair<bool, int32_t>{false, 89})) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply branching resource failed private JIT execution");
    }
    if (!privateRefJitDropProbeValid ||
        privateRefJitNestedDropOrder.size() != branchDropBefore + 6 ||
        !std::equal(privateRefJitNestedDropOrder.begin() + branchDropBefore,
                    privateRefJitNestedDropOrder.end(),
                    std::array<int32_t, 6>{89, 91, 93, 89, 91, 93}.begin()))
        return fail("Ref apply branching resource lost field-order Drop");
    const auto transferredBranchDropBefore =
        privateRefJitNestedDropOrder.size();
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyBranchResourceReturn,
            privateRefJitError, 0, false, std::nullopt, false, false,
            {}, {}, std::pair<bool, int32_t>{false, 89}, 3, true, true)) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply branching resource transfer failed private JIT execution");
    }
    if (!privateRefJitDropProbeValid ||
        privateRefJitNestedDropOrder.size() != transferredBranchDropBefore + 12 ||
        !std::equal(privateRefJitNestedDropOrder.begin() + transferredBranchDropBefore,
                    privateRefJitNestedDropOrder.end(),
                    std::array<int32_t, 12>{89, 91, 93, 89, 91, 93,
                                             89, 91, 93, 89, 91, 93}.begin()))
        return fail("Ref apply branching resource transfer lost field-order Drop");
    if (!tryApplyOversizedBranchReturn->controlFlow ||
        tryApplyOversizedBranchReturn->controlFlow->runtimeRefApplies.size() != 2 ||
        !refApplyCfgVerifier.verify(*tryApplyOversizedBranchReturn->controlFlow,
                                    *tryApplyModule))
        return fail("oversized branching Ref apply fixture did not seal");
    privateRefJitError.clear();
    if (CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
            *tryApplyModule, *tryApplyOversizedBranchReturn,
            privateRefJitError) ||
        privateRefJitError.find(
            "outside the bounded cleanup proof") == std::string::npos)
        return fail("four-node Ref apply owner graph escaped private shape gate");
    privateRefJitError.clear();
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyErrResourceScalarOk,
            privateRefJitError, 1, false, 1, false, false,
            {1}, {}, std::pair<bool, int32_t>{true, 14}, 0)) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply scalar host transfer failed private JIT execution");
    }
    const auto transferredOkDropBefore =
        privateRefJitReturnedOkDropProbeCalls;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyOkResourceReturn,
            privateRefJitError, 1, false, 1, false, false,
            {1}, {}, std::pair<bool, int32_t>{true, 67}, 1)) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply resource Ok host transfer failed private JIT execution");
    }
    if (privateRefJitReturnedOkDropProbeCalls !=
            transferredOkDropBefore + 2 || !privateRefJitDropProbeValid)
        return fail("Ref apply transferred Ok owner was not dropped once per call");
    const auto transferredScalarErrDropBefore =
        privateRefJitReturnedOkDropProbeCalls;
    if (!exercisePrivateRefApplyJit(
            *tryApplyModule, *tryApplyOkResourceScalarErr,
            privateRefJitError, 1, false, 1, false, false,
            {1}, {}, std::pair<bool, int32_t>{false, 13}, 0)) {
        std::cerr << privateRefJitError << '\n';
        return fail("Ref apply scalar Err host transfer failed private JIT execution");
    }
    if (privateRefJitReturnedOkDropProbeCalls !=
        transferredScalarErrDropBefore)
        return fail("Ref apply transferred scalar Err manufactured a resource Drop");
    CodeGenerator blockedTryApply("canonical-ref-try-publication-gate");
    if (blockedTryApply.generate(tryApplyModule.get()) ||
        !std::any_of(blockedTryApply.errors().begin(),
                     blockedTryApply.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "raw-pointer function publication is blocked") !=
                             std::string::npos;
                     }))
        return fail("private Result Ref apply escaped the public codegen gate");
    CodeGenerator blockedNestedEarly("canonical-nested-ref-early-return-gate");
    if (blockedNestedEarly.generate(earlyReturnModule.get()) ||
        !std::any_of(blockedNestedEarly.errors().begin(),
                     blockedNestedEarly.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "requires an explicit Fragment context") !=
                             std::string::npos;
                     })) {
        for (const auto& diagnostic : blockedNestedEarly.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("Slotless nested Ref apply escaped the private body gate");
    }
    auto singleEarlySnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        "export slot checkpoint(value: i32);\n"
        "runtime fn early(selected: RuntimeFragmentRef<checkpoint>) {\n"
        "  apply selected { checkpoint(1) {} return; }\n"
        "}\n",
        "<canonical-single-ref-early-return>");
    if (!singleEarlySnapshot.success())
        return fail("frontend rejected single Ref apply early-return fixture");
    moon::LunaLowerer singleEarlyLowerer;
    auto singleEarlyModule = singleEarlyLowerer.lower(
        *singleEarlySnapshot.program(), *singleEarlySnapshot.symbolTable());
    moon::Sealer singleEarlySealer;
    if (!singleEarlyModule || !singleEarlyLowerer.errors().empty() ||
        !singleEarlySealer.sealFunctionBodies(*singleEarlyModule))
        return fail("single Ref apply early return did not seal");
    moon::FunctionDecl* singleEarly = nullptr;
    for (auto& declaration : singleEarlyModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function && function->name == "early")
            singleEarly = function;
    if (!singleEarly || !exercisePrivateRefApplyJit(
            *singleEarlyModule, *singleEarly, privateRefJitError)) {
        std::cerr << privateRefJitError << '\n';
        return fail("early-return source Ref apply failed private JIT execution");
    }
    auto sequentialEarlySnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            "export slot checkpoint(value: i32);\n"
            "runtime fn sequence_early(selected: RuntimeFragmentRef<checkpoint>) {\n"
            "  apply selected { checkpoint(1) {} checkpoint(2) {} return; }\n"
            "}\n",
            "<canonical-ref-sequential-early>");
    if (!sequentialEarlySnapshot.success())
        return fail("frontend rejected sequential Ref apply early return");
    moon::LunaLowerer sequentialEarlyLowerer;
    auto sequentialEarlyModule = sequentialEarlyLowerer.lower(
        *sequentialEarlySnapshot.program(),
        *sequentialEarlySnapshot.symbolTable());
    moon::Sealer sequentialEarlySealer;
    if (!sequentialEarlyModule || !sequentialEarlyLowerer.errors().empty() ||
        !sequentialEarlySealer.sealFunctionBodies(*sequentialEarlyModule))
        return fail("sequential Ref apply early return did not seal");
    moon::FunctionDecl* sequenceEarly = nullptr;
    for (auto& declaration : sequentialEarlyModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function &&
                function->name == "sequence_early")
            sequenceEarly = function;
    if (!sequenceEarly || !exercisePrivateRefApplyJit(
            *sequentialEarlyModule, *sequenceEarly,
            privateRefJitError, 2)) {
        std::cerr << privateRefJitError << '\n';
        return fail("sequential Ref apply early return failed private JIT execution");
    }
    CodeGenerator blockedSingleEarly("canonical-single-ref-early-return-gate");
    const bool singleEarlyPublished =
        blockedSingleEarly.generate(singleEarlyModule.get());
    if (singleEarlyPublished ||
        !std::any_of(blockedSingleEarly.errors().begin(),
                     blockedSingleEarly.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "1 private Ref apply body(s) verified and discarded") !=
                             std::string::npos;
                     })) {
        for (const auto& diagnostic : blockedSingleEarly.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("single Ref apply early return failed private body proof or public gate");
    }
    auto mixedExitSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        "export slot checkpoint(value: i32);\n"
        "runtime fn mixed(selected: RuntimeFragmentRef<checkpoint>) {\n"
        "  apply selected { if true { return; } checkpoint(1) {} }\n"
        "}\n",
        "<canonical-ref-mixed-exits>");
    if (!mixedExitSnapshot.success())
        return fail("frontend rejected mixed Ref apply exit fixture");
    moon::LunaLowerer mixedExitLowerer;
    auto mixedExitModule = mixedExitLowerer.lower(
        *mixedExitSnapshot.program(), *mixedExitSnapshot.symbolTable());
    moon::Sealer mixedExitSealer;
    if (!mixedExitModule || !mixedExitLowerer.errors().empty() ||
        !mixedExitSealer.sealFunctionBodies(*mixedExitModule))
        return fail("mixed Ref apply exits did not seal");
    CodeGenerator blockedMixedExit("canonical-ref-mixed-exits-gate");
    if (blockedMixedExit.generate(mixedExitModule.get()) ||
        !std::any_of(blockedMixedExit.errors().begin(),
                     blockedMixedExit.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "1 private Ref apply body(s) verified and discarded") !=
                             std::string::npos;
                     }))
        return fail("mixed Ref apply exits failed private proof or public gate");
    moon::FunctionDecl* mixedExit = nullptr;
    for (auto& declaration : mixedExitModule->declarations)
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get()); function && function->name == "mixed")
            mixedExit = function;
    if (!mixedExit || !exercisePrivateRefApplyJit(
            *mixedExitModule, *mixedExit, privateRefJitError, 0)) {
        std::cerr << privateRefJitError << '\n';
        return fail("pre-dispatch early Ref apply failed private JIT execution");
    }
    auto multipleExitSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        "export slot checkpoint(value: i32);\n"
        "runtime fn branches(selected: RuntimeFragmentRef<checkpoint>) {\n"
        "  apply selected {\n"
        "    if true { return; }\n"
        "    if false { return; }\n"
        "    checkpoint(1) {}\n"
        "  }\n"
        "}\n",
        "<canonical-ref-multiple-exits>");
    if (!multipleExitSnapshot.success())
        return fail("frontend rejected multiple Ref apply exits");
    moon::LunaLowerer multipleExitLowerer;
    auto multipleExitModule = multipleExitLowerer.lower(
        *multipleExitSnapshot.program(), *multipleExitSnapshot.symbolTable());
    moon::Sealer multipleExitSealer;
    if (!multipleExitModule || !multipleExitLowerer.errors().empty() ||
        !multipleExitSealer.sealFunctionBodies(*multipleExitModule))
        return fail("multiple Ref apply exits did not seal");
    CodeGenerator blockedMultipleExit("canonical-ref-multiple-exits-gate");
    if (blockedMultipleExit.generate(multipleExitModule.get()) ||
        !std::any_of(blockedMultipleExit.errors().begin(),
                     blockedMultipleExit.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "1 private Ref apply body(s) verified and discarded") !=
                             std::string::npos;
                     }))
        return fail("multiple Ref apply exits failed private proof or public gate");
    std::vector<moon::RegionId> applyEntryOrder;
    for (const auto& edge : earlyFlow->edges)
        applyEntryOrder.insert(applyEntryOrder.end(),
                               edge.enters.begin(), edge.enters.end());
    if (applyEntryOrder != std::vector<moon::RegionId>{
            earlyReturn->controlFlow->runtimeRefApplies[0].region,
            earlyReturn->controlFlow->runtimeRefApplies[1].region})
        return fail("nested Ref apply flow lost outer-before-inner entry order");
    // Disposable LLVM transition proof. This deliberately does not emit the
    // source body or authorize publishing the function as an executable API.
    llvm::LLVMContext contextIr;
    CGHelpers contextHelpers(contextIr);
    llvm::Module contextModule("private.ref.apply.transition", contextIr);
    auto* contextProof = llvm::Function::Create(
        llvm::FunctionType::get(contextHelpers.i32Ty(),
            {contextHelpers.ptrTy(), contextHelpers.ptrTy()}, false),
        llvm::Function::InternalLinkage, "private_ref_apply_transition",
        contextModule);
    auto* proofEntry = llvm::BasicBlock::Create(
        contextIr, "entry", contextProof);
    auto* outerReady = llvm::BasicBlock::Create(
        contextIr, "outer.ready", contextProof);
    auto* innerReady = llvm::BasicBlock::Create(
        contextIr, "inner.ready", contextProof);
    auto* outerFailed = llvm::BasicBlock::Create(
        contextIr, "outer.failed", contextProof);
    auto* innerFailed = llvm::BasicBlock::Create(
        contextIr, "inner.failed", contextProof);
    llvm::IRBuilder<> contextBuilder(proofEntry);
    auto* outerCell = contextBuilder.CreateAlloca(
        contextHelpers.ptrTy(), nullptr, "outer.context.owner");
    auto* innerCell = contextBuilder.CreateAlloca(
        contextHelpers.ptrTy(), nullptr, "inner.context.owner");
    auto* nullContext = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(contextHelpers.ptrTy()));
    contextBuilder.CreateStore(nullContext, outerCell);
    contextBuilder.CreateStore(nullContext, innerCell);
    const auto& exactApplySlot =
        earlyReturn->controlFlow->runtimeRefApplies.front().slot;
    if (contextHelpers.emitRuntimeFragmentRefContextOverride(
            contextBuilder, contextModule, contextProof->getArg(0),
            contextProof->getArg(1), {}, outerCell) ||
        contextHelpers.emitRuntimeFragmentContextDrop(
            contextBuilder, contextModule, nullptr))
        return fail("private LLVM context bridge accepted missing operands");
    auto* outerStatus = contextHelpers.emitRuntimeFragmentRefContextOverride(
        contextBuilder, contextModule, contextProof->getArg(0),
        contextProof->getArg(1), exactApplySlot, outerCell);
    if (!outerStatus)
        return fail("private LLVM outer Ref context derivation failed");
    auto* success = llvm::ConstantInt::get(contextHelpers.i32Ty(), 0);
    contextBuilder.CreateCondBr(
        contextBuilder.CreateICmpEQ(outerStatus, success),
        outerReady, outerFailed);
    contextBuilder.SetInsertPoint(outerFailed);
    contextBuilder.CreateRet(outerStatus);
    contextBuilder.SetInsertPoint(outerReady);
    auto* outerContext = contextBuilder.CreateLoad(
        contextHelpers.ptrTy(), outerCell, "outer.context");
    auto* innerStatus = contextHelpers.emitRuntimeFragmentRefContextOverride(
        contextBuilder, contextModule, outerContext,
        contextProof->getArg(1), exactApplySlot, innerCell);
    if (!innerStatus)
        return fail("private LLVM inner Ref context derivation failed");
    contextBuilder.CreateCondBr(
        contextBuilder.CreateICmpEQ(innerStatus, success),
        innerReady, innerFailed);
    contextBuilder.SetInsertPoint(innerFailed);
    auto* failureDrop = contextHelpers.emitRuntimeFragmentContextDrop(
        contextBuilder, contextModule, outerCell);
    contextBuilder.CreateRet(innerStatus);
    contextBuilder.SetInsertPoint(innerReady);
    std::vector<llvm::CallInst*> returnDrops;
    for (const auto region : earlyFlow->terminals.front().exits) {
        auto* cell = region ==
                earlyReturn->controlFlow->runtimeRefApplies[1].region
            ? innerCell : region ==
                earlyReturn->controlFlow->runtimeRefApplies[0].region
                ? outerCell : nullptr;
        if (!cell)
            return fail("private LLVM proof saw an unknown Ref context region");
        returnDrops.push_back(contextHelpers.emitRuntimeFragmentContextDrop(
            contextBuilder, contextModule, cell));
    }
    contextBuilder.CreateRet(success);
    const auto hasExactIdentity = [&](const llvm::CallInst* call,
                                      unsigned index,
                                      const std::string& expected) {
        const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(
            call->getArgOperand(index));
        const auto* data = global
            ? llvm::dyn_cast<llvm::ConstantDataArray>(global->getInitializer())
            : nullptr;
        return data && data->isCString() && data->getAsCString() == expected;
    };
    if (!contextProof->hasInternalLinkage() || !failureDrop ||
        failureDrop->getArgOperand(0) != outerCell ||
        returnDrops.size() != 2 || !returnDrops[0] || !returnDrops[1] ||
        returnDrops[0]->getArgOperand(0) != innerCell ||
        returnDrops[1]->getArgOperand(0) != outerCell ||
        outerStatus->getCalledFunction() !=
            contextModule.getFunction(
                "luna_compiler_fragment_context_override_from_ref") ||
        innerStatus->getCalledFunction() !=
            outerStatus->getCalledFunction() ||
        !hasExactIdentity(outerStatus, 2, exactApplySlot.symbol.value) ||
        !hasExactIdentity(outerStatus, 3, exactApplySlot.contract.value) ||
        !hasExactIdentity(innerStatus, 2, exactApplySlot.symbol.value) ||
        !hasExactIdentity(innerStatus, 3, exactApplySlot.contract.value) ||
        llvm::verifyModule(contextModule))
        return fail("private LLVM Ref context proof lost status or cleanup order");

    auto contextRefSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        "export slot checkpoint(value: i32);\n"
        "runtime fn dispatch(selected: RuntimeFragmentRef<checkpoint>) {\n"
        "  checkpoint(1) {}\n"
        "}\n"
        "runtime fn relay(selected: RuntimeFragmentRef<checkpoint>) {\n"
        "  dispatch(selected);\n"
        "}\n",
        "<canonical-ref-context-effect>");
    if (!contextRefSnapshot.success())
        return fail("frontend rejected context-bearing Ref entry preparation");
    moon::LunaLowerer contextRefLowerer;
    auto contextRefModule = contextRefLowerer.lower(
        *contextRefSnapshot.program(), *contextRefSnapshot.symbolTable());
    if (!contextRefModule || !contextRefLowerer.errors().empty())
        return fail("context-bearing Ref entry failed private MoonIR lowering");
    moon::Sealer contextRefSealer;
    if (!contextRefSealer.sealFunctionBodies(*contextRefModule))
        return fail("context-bearing Ref entry did not seal a canonical CFG");
    moon::FunctionDecl* contextDispatch = nullptr;
    moon::FunctionDecl* contextRelay = nullptr;
    for (auto& declaration : contextRefModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (!function) continue;
        if (function->name == "dispatch") contextDispatch = function;
        if (function->name == "relay") contextRelay = function;
    }
    if (!contextDispatch || !contextRelay ||
        !contextDispatch->requiresFragmentContext ||
        !contextRelay->requiresFragmentContext)
        return fail("runtime Slot/Ref entry lost direct or transitive context effect");
    CodeGenerator blockedContextRef("canonical-ref-context-gate");
    if (blockedContextRef.generate(contextRefModule.get()) ||
        std::any_of(blockedContextRef.errors().begin(),
                    blockedContextRef.errors().end(),
                    [](const auto& diagnostic) {
                        return diagnostic.message.find(
                            "private RuntimeFragmentRef unit ingress proof failed") !=
                            std::string::npos;
                    }))
        return fail("context-bearing Ref entry escaped or failed private proof");
    contextRelay->requiresFragmentContext = false;
    CodeGenerator forgedContextRef("canonical-ref-forged-context-gate");
    const bool forgedContextPublished =
        forgedContextRef.generate(contextRefModule.get());
    contextRelay->requiresFragmentContext = true;
    if (forgedContextPublished ||
        !std::any_of(forgedContextRef.errors().begin(),
                     forgedContextRef.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "private RuntimeFragmentRef unit ingress proof failed for 'relay'") !=
                             std::string::npos &&
                             diagnostic.message.find(
                                 "context effect differs from the sealed CFG fixed point") !=
                             std::string::npos;
                     }))
        return fail("forged transitive Ref context effect passed private proof");

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
