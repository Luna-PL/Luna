#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace moon {
struct Module;
struct FunctionDecl;
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

bool deriveNativeRefUnitSourceFacts(
    const moon::Module& program, const moon::FunctionDecl& function,
    NativeRefUnitSourceFacts& facts, std::string& error);

} // namespace luna::codegen
