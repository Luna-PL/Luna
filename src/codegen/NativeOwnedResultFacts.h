#pragma once

#include <cstdint>
#include <string>
#include <tuple>

namespace moon {
struct Module;
struct FunctionDecl;
}

namespace luna::codegen {

// Frozen source facts required by the candidate Native v3 Ref/Result row.
// This deliberately has no entry pointer or wrapper linkage: the current
// private transfer wrapper is not the proposed host ABI.
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

bool deriveNativeOwnedResultSourceFacts(
    const moon::Module& program, const moon::FunctionDecl& function,
    NativeOwnedResultSourceFacts& facts, std::string& error);

} // namespace luna::codegen
