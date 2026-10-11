#pragma once

#include <array>
#include <cstdint>
#include <string>

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

} // namespace luna::codegen
