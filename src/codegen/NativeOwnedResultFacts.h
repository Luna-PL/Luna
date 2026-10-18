#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "runtime/NativeArtifactABI.h"

namespace moon {
struct Module;
struct FunctionDecl;
}
namespace llvm {
class Module;
}

namespace luna::codegen {

// Frozen source facts required by the candidate Native v3 Ref/Result row.
// Source facts deliberately have no entry pointer or wrapper linkage. The
// separate generated-entry proof pairs those identities after IR verification.
struct NativeOwnedResultSourceFacts {
    std::string functionSymbolId;
    std::string functionContractId;
    std::string sourceLinkageName;
    std::string refSlotSymbolId;
    std::string refSlotContractId;
    std::string resultTypeId;
    std::string errorTypeId;
    std::string errorAbiLayoutId;
    std::string errorDropSymbolId;
    std::string errorDropContractId;
    uint64_t errorValueSize = 0;
    uint64_t errorValueAlignment = 0;

    friend bool operator==(const NativeOwnedResultSourceFacts& left,
                           const NativeOwnedResultSourceFacts& right) {
        return std::tie(left.functionSymbolId, left.functionContractId,
                   left.sourceLinkageName, left.refSlotSymbolId,
                   left.refSlotContractId, left.resultTypeId,
                   left.errorTypeId, left.errorAbiLayoutId,
                   left.errorDropSymbolId, left.errorDropContractId,
                   left.errorValueSize, left.errorValueAlignment) ==
               std::tie(right.functionSymbolId, right.functionContractId,
                   right.sourceLinkageName, right.refSlotSymbolId,
                   right.refSlotContractId, right.resultTypeId,
                   right.errorTypeId, right.errorAbiLayoutId,
                   right.errorDropSymbolId, right.errorDropContractId,
                   right.errorValueSize, right.errorValueAlignment);
    }
};

// Pointer-free producer candidate for one v3 row. The wrapper name remains a
// claim until generated IR proves that it implements the sealed source body.
struct NativeOwnedResultDescriptorV3Candidate {
    NativeOwnedResultSourceFacts source;
    std::string wrapperLinkageName;

    std::array<std::string, 10> descriptorIdentities() const;
    std::string canonicalRow() const;
};

// Test-only pointer-free pairing of a verified generated host entry with its
// sealed source facts. A real v3 row still needs artifact and loader binding.
struct NativeOwnedResultEntryProof {
    NativeOwnedResultSourceFacts source;
    std::string entryLinkageName;
    std::string injectionLinkageName;
    std::string dropLinkageName;
    uint32_t entryAbi = LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1;
    uint32_t statusDomain = LUNA_NATIVE_STATUS_DOMAIN_REF_RESULT_OWNER_V1;
};

bool deriveNativeOwnedResultSourceFacts(
    const moon::Module& program, const moon::FunctionDecl& function,
    NativeOwnedResultSourceFacts& facts, std::string& error);

bool deriveNativeOwnedResultDescriptorV3Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const std::string& wrapperLinkageName,
    NativeOwnedResultDescriptorV3Candidate& candidate, std::string& error);

// Proves the exported six-argument entry calls one defined seven-argument
// host wrapper with the private injection argument fixed to zero. The host
// wrapper's ownership and failure CFG require their separate verifier.
bool derivePairedNativeOwnedResultDescriptorV3Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const llvm::Module& generated, const std::string& wrapperLinkageName,
    const std::string& hostLinkageName,
    NativeOwnedResultDescriptorV3Candidate& candidate, std::string& error);

// Proves the owned-error adoption fork: the identified final tag-store block
// is reached only from successful adoption, while the failure edge drops the
// same local owner cell and returns the versioned failure status.
bool verifyNativeOwnedResultHostAdoption(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& dropLinkageName, std::string& error);

// Proves the host's single transfer call uses local scratch outputs and a
// zero injection argument. A failed transfer returns its status directly;
// only success reaches the branch's success continuation.
bool verifyNativeOwnedResultHostTransferFork(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName, std::string& error);

// Proves that a successful transfer commits the scalar output and tag only
// after tag 1 and a null local owner. The resource and invalid-result paths
// have separate checks.
bool verifyNativeOwnedResultHostScalarCommit(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName, std::string& error);

// Proves the resource tag's non-null owner gate and zero-injection path to
// adoption, plus both invalid-resource paths. A present invalid owner is
// dropped from the transfer's local owner cell before status 2 is returned.
bool verifyNativeOwnedResultHostResourceValidation(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName,
    const std::string& dropLinkageName, std::string& error);

// Composes the host branch verifiers and closes the generated control-flow
// graph: every block is reachable, and scratch cells, calls, memory effects
// and returns match the checked status paths.
bool verifyNativeOwnedResultHostClosedGraph(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName,
    const std::string& dropLinkageName, std::string& error);

// Proves that transfer is reachable only through the caller owner-cell-empty
// check and the Runtime code-lease-live check. Both failed gates return their
// versioned status. The preceding output validity expression is separate.
bool verifyNativeOwnedResultHostOwnerLeaseGates(
    const llvm::Module& generated, const std::string& hostLinkageName,
    const std::string& transferLinkageName, std::string& error);

// Proves the entry success edge requires all three caller output pointers to
// be non-null and the other edge returns the invalid-output status. Alignment
// and disjointness remain separate checks.
bool verifyNativeOwnedResultHostOutputNonNull(
    const llvm::Module& generated, const std::string& hostLinkageName,
    std::string& error);

// Proves the entry success edge also requires ABI alignment of each output
// pointer. Pairwise non-overlap remains a separate check.
bool verifyNativeOwnedResultHostOutputAlignment(
    const llvm::Module& generated, const std::string& hostLinkageName,
    std::string& error);

// Proves all three output regions are pairwise disjoint on the entry success
// edge, using their generated ABI sizes and unsigned address-gap checks.
bool verifyNativeOwnedResultHostOutputDisjointness(
    const llvm::Module& generated, const std::string& hostLinkageName,
    std::string& error);

// Checks the private transfer's injected owned-error failure path against
// source-derived cleanup calls and physical owner allocation layout. The
// failure block has no scratch writes or other unrecognized side effects;
// null owner has only the resource gate as predecessor. Other transfer
// statuses remain separate checks.
bool verifyNativeOwnedResultTransferInjectedCleanup(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    const std::vector<std::string>& expectedCleanupCalls,
    uint64_t allocationSize, uint64_t allocationAlignment,
    uint32_t invalidResourceStatus, uint32_t injectedFailureStatus,
    std::string& error);

// Preorder view of the admitted owned Struct tree. Entry zero is the root;
// each later entry names its parent and pointer-field offset in that parent.
struct NativeOwnedResultCleanupNode {
    std::string dropLinkageName;
    uint32_t parentIndex;
    uint64_t fieldOffset;
    uint64_t allocationSize;
    uint64_t allocationAlignment;
};

enum class NativeOwnedResultCleanupStepKind : uint8_t {
    Drop,
    ReadI32,
    ReadOwner,
    Deallocate,
};

struct NativeOwnedResultCleanupStep {
    NativeOwnedResultCleanupStepKind kind;
    uint32_t ownerIndex;
    uint64_t fieldOffset;
    uint32_t childIndex;
};

// Binds every nested Drop and deallocation to a pointer loaded from the
// source-derived field offset of its parent owner, and audits all admitted
// field reads in declaration order.
bool verifyNativeOwnedResultTransferCleanupProvenance(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    const std::vector<std::string>& expectedCleanupCalls,
    const std::vector<NativeOwnedResultCleanupNode>& cleanupNodes,
    const std::vector<NativeOwnedResultCleanupStep>& cleanupSteps,
    uint32_t invalidResourceStatus, uint32_t injectedFailureStatus,
    std::string& error);

// Checks the owned-error transfer's scalar Ok arm from body tag dispatch
// through injection failure or a success commit that writes tag and scalar.
bool verifyNativeOwnedResultTransferScalarFork(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    uint32_t injectedFailureStatus, uint32_t successStatus,
    std::string& error);

// Checks the owned-error transfer's resource Err commit from body tag and
// non-null owner gates through the zero-injection edge to tag and owner stores.
bool verifyNativeOwnedResultTransferResourceCommit(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    uint32_t successStatus, std::string& error);

// Checks the context and exact borrowed-Ref status switches before the one
// source-body call. Entry output and owner-cell gates are separate proof work.
bool verifyNativeOwnedResultTransferContextRefPreflight(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    uint32_t invalidContextStatus, uint32_t invalidHandleStatus,
    uint32_t invalidTargetStatus, uint32_t unexpectedStatus,
    std::string& error);

// Checks the transfer entry's output pointer guards and empty owner cell
// before context preflight. Both rejection edges return invalid output.
bool verifyNativeOwnedResultTransferOutputOwnerGates(
    const llvm::Module& generated, const std::string& transferLinkageName,
    uint32_t invalidOutputStatus, std::string& error);

// Composes the private transfer proofs and closes its reachable status graph.
// Only the verified success arms write output cells; cleanup may read nested
// resource fields but may not allocate, write, or add calls beyond the
// source-derived cleanup sequence.
struct NativeOwnedResultTransferStatuses {
    uint32_t success;
    uint32_t invalidOutput;
    uint32_t invalidResource;
    uint32_t injectedFailure;
    uint32_t invalidContext;
    uint32_t invalidHandle;
    uint32_t invalidTarget;
    uint32_t unexpectedCheck;
};

bool verifyNativeOwnedResultTransferClosedGraph(
    const llvm::Module& generated, const std::string& transferLinkageName,
    const NativeOwnedResultSourceFacts& source,
    const std::vector<std::string>& expectedCleanupCalls,
    const std::vector<NativeOwnedResultCleanupNode>& cleanupNodes,
    const std::vector<NativeOwnedResultCleanupStep>& cleanupSteps,
    const NativeOwnedResultTransferStatuses& statuses,
    std::string& error);

} // namespace luna::codegen
