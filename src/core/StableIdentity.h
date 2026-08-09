#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace luna::identity {

template <typename Tag>
struct StableId {
    std::string value;

    bool empty() const { return value.empty(); }
    bool operator==(const StableId& other) const {
        return value == other.value;
    }
    bool operator!=(const StableId& other) const {
        return !(*this == other);
    }
};

struct TypeIdTag;
struct ShapeIdTag;
struct SymbolIdTag;
struct ContractIdTag;
struct AbiLayoutIdTag;

using TypeId = StableId<TypeIdTag>;
using ShapeId = StableId<ShapeIdTag>;
using SymbolId = StableId<SymbolIdTag>;
using ContractId = StableId<ContractIdTag>;
using AbiLayoutId = StableId<AbiLayoutIdTag>;

inline uint64_t stableIdentityHash(const std::string& canonical) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <typename Id>
inline Id compactIdentity(const char* prefix,
                          const std::string& canonical) {
    std::ostringstream output;
    output << prefix << std::hex << std::setw(16) << std::setfill('0')
           << stableIdentityHash(canonical);
    return {output.str()};
}

inline SymbolId symbolIdFromCanonical(const std::string& canonical) {
    return compactIdentity<SymbolId>("symbol_", canonical);
}

inline ContractId contractIdFromCanonical(const std::string& canonical) {
    return compactIdentity<ContractId>("contract_", canonical);
}

inline AbiLayoutId abiLayoutIdFromCanonical(const std::string& canonical) {
    return compactIdentity<AbiLayoutId>("abi_", canonical);
}

} // namespace luna::identity
