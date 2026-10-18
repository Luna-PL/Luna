#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <tuple>

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

} // namespace luna::codegen
