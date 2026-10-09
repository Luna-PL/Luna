#pragma once

#include <cstdint>
#include <string>
#include <tuple>

#include "runtime/NativeArtifactABI.h"

namespace moon {
struct Module;
struct FunctionDecl;
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

} // namespace luna::codegen
