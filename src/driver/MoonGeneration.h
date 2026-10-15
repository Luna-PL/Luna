#pragma once

#include "runtime/MoonRuntime.h"
#include "moonir/MoonIRModule.h"
#include "codegen/NativeRefUnitFacts.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef LUNA_PRIVATE_REF_JIT_TEST
class LunaPrivateRefUnitApplyLoadedEntry;
#endif

namespace luna::runtime {
struct RuntimeSlotRequirement;
}

namespace luna::driver {

// Compiler-owned LLVM middle-end profile; not a source keyword or container
// capability. The legacy/default path remains O0.
enum class MoonJitOptimization { O0, O2, O3 };

// Stages a host-matched, fully verified Moon Container with a retained ORC JIT
// lease for function publications and descriptor-backed non-function exports.
// This compiler-owned artifact adapter feeds the public MoonRuntime control
// plane; packaging it as a standalone host SDK is outside the 0.3 surface.
// Explicit dependency Slot evidence is checked before JIT/staging and even
// before a load-once cache hit. It does not select or activate any Fragment.
// The LLVM version/IR profile/ORC policy form a separate materialization key;
// load-once rejects different keys rather than silently reusing old code.
bool stageVerifiedMoonGeneration(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    const luna::runtime::GenerationInitializer& initializer,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::string& error,
    const moon::SlotPublicationDependencies& dependencies = {},
    MoonJitOptimization optimization = MoonJitOptimization::O0);

bool loadVerifiedMoonGenerationOnce(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    luna::runtime::MoonRuntime::PinnedGeneration& loaded,
    std::string& error,
    const moon::SlotPublicationDependencies& dependencies = {},
    MoonJitOptimization optimization = MoonJitOptimization::O0);

struct VerifiedMoonSlotOwnerState;
struct VerifiedMoonRefUnitPreparationState;
class VerifiedMoonRefUnitPreparation;
class VerifiedMoonRefUnitGeneration;
class VerifiedMoonImportedSlotRefUnitProof;
class VerifiedMoonImportedSlotGeneration;
#ifdef LUNA_PRIVATE_REF_JIT_TEST
struct PrivatePreparedRefGenerationForTest;
#endif

// Internal loader-issued owner provenance. A fresh loader-owned Runtime stages
// and loads the verified artifact, so host-staged generations cannot be paired
// with unrelated bytes. This is not a standalone host SDK or Ref publication.
class VerifiedMoonSlotOwner {
public:
    explicit operator bool() const { return state_ != nullptr; }
    uint64_t generationId() const;
    // The caller must verify the consumer artifact and its direct dependency.
    // Pins the owner only when its verified export, supplied import record and
    // loaded public-control binding describe the same Slot for this target.
    bool pinImportedSlot(
        const moon::DeclarationRecord& importedSlot,
        const std::string& expectedTargetTriple,
        const std::string& expectedDataLayout,
        luna::runtime::MoonRuntime::PinnedGeneration& pinned,
        std::string& error) const;

private:
    friend bool loadVerifiedMoonSlotOwner(
        const std::vector<uint8_t>&, const std::string&,
        const std::string&, VerifiedMoonSlotOwner&, std::string&);
    friend bool loadVerifiedMoonRefUnitPreparation(
        const std::vector<uint8_t>&, const VerifiedMoonSlotOwner&,
        const std::string&, const std::string&,
        VerifiedMoonRefUnitPreparation&, std::string&);
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    friend bool stagePrivateNativeRefUnitPreparationForTest(
        luna::runtime::MoonRuntime&, const std::vector<uint8_t>&,
        const VerifiedMoonSlotOwner&, const std::string&,
        const std::string&,
        luna::runtime::MoonRuntime::StagedGeneration&,
        std::weak_ptr<LunaPrivateRefUnitApplyLoadedEntry>&,
        std::weak_ptr<PrivatePreparedRefGenerationForTest>&,
        std::string&);
#endif
    std::shared_ptr<const VerifiedMoonSlotOwnerState> state_;
};

bool loadVerifiedMoonSlotOwner(
    const std::vector<uint8_t>& ownerContainerBytes,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    VerifiedMoonSlotOwner& owner,
    std::string& error);

// Compiler-internal provenance for a canonical 0.4 Ref/unit consumer. This
// retains the decoded artifact, exact verified owner pin and isolated JIT
// code without callable bindings or Runtime candidate publication.
class VerifiedMoonRefUnitPreparation {
public:
    explicit operator bool() const { return state_ != nullptr; }
    uint64_t ownerGenerationId() const;
    std::optional<luna::codegen::NativeRefUnitSourceFacts> facts() const;
    // Artifact identity is necessary but does not replace the module lease check.
    bool matchesConsumer(
        const luna::runtime::MoonRuntime::PinnedGeneration& consumer) const;

private:
    friend class VerifiedMoonImportedSlotGeneration;
    friend class VerifiedMoonRefUnitGeneration;
    friend class VerifiedMoonImportedSlotRefUnitProof;
    friend bool loadVerifiedMoonRefUnitGenerationOnce(
        const VerifiedMoonRefUnitPreparation&,
        VerifiedMoonRefUnitGeneration&, std::string&);
    friend bool pinVerifiedMoonImportedSlotRefUnitProof(
        const VerifiedMoonRefUnitGeneration&, const std::string&,
        const std::string&, const luna::runtime::RuntimeSlotRequirement&,
        VerifiedMoonImportedSlotRefUnitProof&, std::string&);
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    friend class PrivateImportedSlotRefUnitCandidateForTest;
#endif
    friend bool loadVerifiedMoonRefUnitPreparation(
        const std::vector<uint8_t>&, const VerifiedMoonSlotOwner&,
        const std::string&, const std::string&,
        VerifiedMoonRefUnitPreparation&, std::string&);
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    friend bool stagePrivateNativeRefUnitPreparationForTest(
        luna::runtime::MoonRuntime&, const std::vector<uint8_t>&,
        const VerifiedMoonSlotOwner&, const std::string&,
        const std::string&,
        luna::runtime::MoonRuntime::StagedGeneration&,
        std::weak_ptr<LunaPrivateRefUnitApplyLoadedEntry>&,
        std::weak_ptr<PrivatePreparedRefGenerationForTest>&,
        std::string&);
#endif
    std::shared_ptr<const VerifiedMoonRefUnitPreparationState> state_;
};

bool loadVerifiedMoonRefUnitPreparation(
    const std::vector<uint8_t>& consumerContainerBytes,
    const VerifiedMoonSlotOwner& owner,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    VerifiedMoonRefUnitPreparation& preparation,
    std::string& error);

// Internal, inert consumer generation. A loader-owned Runtime loads the exact
// preparation lease without publishing bindings or a Ref/unit candidate.
// Generation IDs are scoped to that Runtime; use matchesPreparation for proof.
class VerifiedMoonRefUnitGeneration {
public:
    explicit operator bool() const {
        return static_cast<bool>(preparation_) && static_cast<bool>(consumer_);
    }
    uint64_t generationId() const;
    uint64_t ownerGenerationId() const;
    std::optional<luna::codegen::NativeRefUnitSourceFacts> facts() const;
    bool matchesPreparation(const VerifiedMoonRefUnitPreparation& preparation) const;

private:
    friend class VerifiedMoonImportedSlotGeneration;
    friend class VerifiedMoonImportedSlotRefUnitProof;
    friend bool loadVerifiedMoonRefUnitGenerationOnce(
        const VerifiedMoonRefUnitPreparation&,
        VerifiedMoonRefUnitGeneration&, std::string&);
    friend bool pinVerifiedMoonImportedSlotRefUnitProof(
        const VerifiedMoonRefUnitGeneration&, const std::string&,
        const std::string&, const luna::runtime::RuntimeSlotRequirement&,
        VerifiedMoonImportedSlotRefUnitProof&, std::string&);
    VerifiedMoonRefUnitPreparation preparation_;
    luna::runtime::MoonRuntime::PinnedGeneration consumer_;
};

bool loadVerifiedMoonRefUnitGenerationOnce(
    const VerifiedMoonRefUnitPreparation& preparation,
    VerifiedMoonRefUnitGeneration& loaded,
    std::string& error);

// Internal imported-Slot admission proof. The verified consumer and its owner
// binding remain pinned, but this proof has no callable or Runtime candidate.
class VerifiedMoonImportedSlotRefUnitProof {
public:
    explicit operator bool() const { return static_cast<bool>(consumer_); }
    uint64_t consumerGenerationId() const;
    uint64_t ownerGenerationId() const;
    std::optional<luna::codegen::NativeRefUnitSourceFacts> facts() const;
    std::optional<luna::runtime::GenerationRefUnitCandidate> candidateFacts() const;
    bool matchesPreparation(const VerifiedMoonRefUnitPreparation& preparation) const;
    bool loadBindingOnce(VerifiedMoonImportedSlotGeneration& loaded,
                         std::string& error) const;

private:
    friend class VerifiedMoonImportedSlotGeneration;
    friend bool pinVerifiedMoonImportedSlotRefUnitProof(
        const VerifiedMoonRefUnitGeneration&, const std::string&,
        const std::string&, const luna::runtime::RuntimeSlotRequirement&,
        VerifiedMoonImportedSlotRefUnitProof&, std::string&);
    VerifiedMoonRefUnitGeneration consumer_;
};

// Loader-owned proxy for a verified imported Slot. It retains the owner and
// consumer proofs and publishes only a noncallable Slot binding internally.
class VerifiedMoonImportedSlotGeneration {
public:
    explicit operator bool() const {
        return proof_ && runtime_ && static_cast<bool>(generation_);
    }
    uint64_t generationId() const;
    bool hasImportedSlotBinding() const;
    bool hasMetadataCandidate() const;
    bool hasTypedCaller() const;
    std::optional<luna::runtime::GenerationRefUnitCandidate> candidateFacts() const;
    // Activates a new internal generation with the same Slot proxy and one
    // pointer-free Ref/unit candidate. No typed caller is installed.
    bool admitMetadataCandidateOnce(std::string& error);
    // Activates a further internal generation whose typed adapter holds the
    // exact verified preparation and owner leases for the whole call.
    bool installTypedCallerOnce(std::string& error);
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    luna::runtime::MoonRuntime::PinnedRefUnitCandidate pinCandidateForTest() const;
#endif

private:
    friend class VerifiedMoonImportedSlotRefUnitProof;
    std::shared_ptr<const VerifiedMoonImportedSlotRefUnitProof> proof_;
    std::shared_ptr<luna::runtime::MoonRuntime> runtime_;
    luna::runtime::MoonRuntime::PinnedGeneration generation_;
    bool typedCallerInstalled_ = false;
};

// Compiler-internal complete load of a verified imported-Slot Ref/unit source.
// The output is assigned only after preparation, proof admission and typed
// caller activation all succeed; it does not publish a public Runtime row.
bool loadVerifiedMoonImportedSlotRefUnitOnce(
    const std::vector<uint8_t>& consumerContainerBytes,
    const VerifiedMoonSlotOwner& owner,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    const std::string& sourceSymbolId,
    const std::string& sourceContractId,
    const luna::runtime::RuntimeSlotRequirement& slot,
    VerifiedMoonImportedSlotGeneration& loaded,
    std::string& error);

bool pinVerifiedMoonImportedSlotRefUnitProof(
    const VerifiedMoonRefUnitGeneration& consumer,
    const std::string& sourceSymbolId,
    const std::string& sourceContractId,
    const luna::runtime::RuntimeSlotRequirement& slot,
    VerifiedMoonImportedSlotRefUnitProof& proof,
    std::string& error);

#ifdef LUNA_PRIVATE_REF_JIT_TEST
// Compiler-private typed candidate. The consumer pin must own exactly the
// preparation lease that holds its decoded facts, owner pin and JIT entry.
class PrivateImportedSlotRefUnitCandidateForTest {
public:
    explicit operator bool() const {
        return static_cast<bool>(consumer_) && !preparation_.expired();
    }
    uint64_t consumerGenerationId() const;
    uint64_t ownerGenerationId() const;
    std::optional<luna::codegen::NativeRefUnitSourceFacts> facts() const;
    bool call(const luna::runtime::RuntimeFragmentExecutionContext& parent,
              const luna::runtime::RuntimeFragmentRefHandle& reference,
              int32_t& status, std::string& error) const;

private:
    friend bool pinPrivateImportedSlotRefUnitCandidateForTest(
        const luna::runtime::MoonRuntime::PinnedGeneration&,
        const std::weak_ptr<PrivatePreparedRefGenerationForTest>&,
        const std::string&, const std::string&,
        const luna::runtime::RuntimeSlotRequirement&,
        PrivateImportedSlotRefUnitCandidateForTest&, std::string&);
    luna::runtime::MoonRuntime::PinnedGeneration consumer_;
    std::weak_ptr<PrivatePreparedRefGenerationForTest> preparation_;
};

bool pinPrivateImportedSlotRefUnitCandidateForTest(
    const luna::runtime::MoonRuntime::PinnedGeneration& consumerGeneration,
    const std::weak_ptr<PrivatePreparedRefGenerationForTest>& preparation,
    const std::string& sourceSymbolId, const std::string& sourceContractId,
    const luna::runtime::RuntimeSlotRequirement& slot,
    PrivateImportedSlotRefUnitCandidateForTest& candidate,
    std::string& error);

// Quarantined test-only staging of a prepared 0.4 Ref/unit consumer. The
// retained entry is reachable only through the returned weak reference while
// the staged or pinned generation owns its module lease. The loader-issued
// owner proof supplies its exact exported Slot and pinned generation. No
// callable binding or Ref/unit candidate is published to the Runtime.
bool stagePrivateNativeRefUnitPreparationForTest(
    luna::runtime::MoonRuntime& runtime,
    const std::vector<uint8_t>& containerBytes,
    const VerifiedMoonSlotOwner& owner,
    const std::string& expectedTargetTriple,
    const std::string& expectedDataLayout,
    luna::runtime::MoonRuntime::StagedGeneration& staged,
    std::weak_ptr<LunaPrivateRefUnitApplyLoadedEntry>& entry,
    std::weak_ptr<PrivatePreparedRefGenerationForTest>& preparation,
    std::string& error);

// Drops only the separate test JIT entry. The typed candidate must still call
// through the verified preparation's ordinary-build code lease.
bool discardPrivateRefUnitTestEntryForTest(
    const std::weak_ptr<PrivatePreparedRefGenerationForTest>& preparation,
    std::string& error);
#endif

} // namespace luna::driver
