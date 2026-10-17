#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "driver/NativeArtifact.h"

namespace moon {
struct Module;
struct FunctionDecl;
}
namespace llvm {
class Module;
}

namespace luna::codegen {

// Verified source identity for the candidate borrowed Ref/unit host profile.
// No generated wrapper symbol, entry address or artifact claim belongs here.
struct NativeRefUnitSourceFacts {
    std::string moduleName;
    std::string packageId;
    std::string functionSymbolId;
    std::string functionContractId;
    std::string functionTypeId;
    std::string sourceLinkageName;
    std::string refTypeId;
    std::string slotSymbolId;
    std::string slotContractId;
    std::string slotTypeId;
    uint32_t contextEffect = 0;

    std::array<std::string, 10> identities() const {
        return {moduleName, packageId, functionSymbolId,
                functionContractId, functionTypeId, sourceLinkageName,
                refTypeId, slotSymbolId, slotContractId, slotTypeId};
    }
};

// Preparation-only identity for a two-borrowed-Ref/unit source. This is a
// distinct profile and cannot be passed as a one-Ref host candidate fact.
struct NativeRefPairUnitSourceFacts {
    std::string moduleName;
    std::string packageId;
    std::string functionSymbolId;
    std::string functionContractId;
    std::string functionTypeId;
    std::string sourceLinkageName;
    std::string refTypeId;
    std::string slotSymbolId;
    std::string slotContractId;
    std::string slotTypeId;
    uint32_t contextEffect = 0;

    std::array<std::string, 10> identities() const {
        return {moduleName, packageId, functionSymbolId,
                functionContractId, functionTypeId, sourceLinkageName,
                refTypeId, slotSymbolId, slotContractId, slotTypeId};
    }
};

// Producer-side candidate row. The source facts are rederived from the sealed
// function; the wrapper linkage remains a separate claim until generated IR
// proves that it calls that exact source body. No pointer or export is emitted.
struct NativeRefUnitDescriptorV4Candidate {
    NativeRefUnitSourceFacts source;
    std::string wrapperLinkageName;

    std::array<std::string, 10> descriptorIdentities() const;
    std::string canonicalRow() const;
};

bool deriveNativeRefUnitSourceFacts(
    const moon::Module& program, const moon::FunctionDecl& function,
    NativeRefUnitSourceFacts& facts, std::string& error);

bool deriveNativeRefPairUnitSourceFacts(
    const moon::Module& program, const moon::FunctionDecl& function,
    NativeRefPairUnitSourceFacts& facts, std::string& error);

// After Native-only MoonIR preparation, admits at most one public Ref/unit
// source and rejects other Ref uses before ordinary codegen is reached.
bool verifyNativeRefUnitModulePreparation(
    const moon::Module& program, std::string& error);

bool deriveNativeRefUnitDescriptorV4Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const std::string& wrapperLinkageName,
    NativeRefUnitDescriptorV4Candidate& candidate, std::string& error);

// Pairs a freshly derived row with defined LLVM body/wrapper symbols and
// exact borrowed target constants. Status CFG and failure containment remain
// separate publication checks.
bool derivePairedNativeRefUnitDescriptorV4Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const llvm::Module& generated, const std::string& wrapperLinkageName,
    NativeRefUnitDescriptorV4Candidate& candidate, std::string& error);

// Proves the five preflight status paths and that only successful context and
// Ref checks reach the body. Body execution failure remains a separate gate.
bool derivePreflightNativeRefUnitDescriptorV4Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    const llvm::Module& generated, const std::string& wrapperLinkageName,
    NativeRefUnitDescriptorV4Candidate& candidate, std::string& error);

// Builds the guarded C status entry from one sealed Ref/unit Apply body, then
// verifies its exact source/body pairing and preflight CFG. The caller still
// owns publication, artifact proof and lifetime policy.
bool emitNativeRefUnitStatusWrapper(
    const moon::Module& program, const moon::FunctionDecl& function,
    llvm::Module& generated, const std::string& wrapperLinkageName,
    std::string& error);

// Derives the v4 proof row only for a sealed public function whose exact
// target Slot is already present in the proof-bound v1 public registry. The
// caller appends the returned row before embedding the artifact proof.
bool deriveNativeRefUnitV4ProofClaim(
    const moon::Module& program, const moon::FunctionDecl& function,
    const llvm::Module& generated, const std::string& wrapperLinkageName,
    const std::vector<luna::driver::NativeExportSpec>& v1Exports,
    const luna::driver::NativeProofSpec& proof,
    std::string& claim, std::string& error);

// Coordinates the one-row v4 descriptor with its canonical proof claim.
// On failure proof is unchanged; the caller owns proof embedding, artifact
// sealing and the still-gated public Ref execution path.
bool emitNativeRefUnitV4ClaimedDescriptorCandidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    llvm::Module& generated, const std::string& wrapperLinkageName,
    const std::vector<luna::driver::NativeExportSpec>& v1Exports,
    luna::driver::NativeProofSpec& proof, std::string& error);

// Emits one v4 candidate only after rederiving its sealed source and exact
// generated preflight. The caller must separately prove the body failure path
// and bind the resulting image to a Native proof before publication.
bool emitNativeRefUnitDescriptorV4Candidate(
    const moon::Module& program, const moon::FunctionDecl& function,
    llvm::Module& generated, const std::string& wrapperLinkageName,
    const std::string& packageVersion, const std::string& targetAbi,
    const std::string& compilerIdentity, std::string& error);

} // namespace luna::codegen
