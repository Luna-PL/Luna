#pragma once

#include "RuntimeDescriptorABI.h"

#include <cstdint>
#include <sstream>
#include <string>

namespace luna::runtime {

inline std::string runtimeDescriptorRegistrySymbol(
    const std::string& moduleId) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : moduleId) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream symbol;
    symbol << "__moon_runtime_registry_" << std::hex << hash;
    return symbol.str();
}

// A validated view over one lease-owned in-memory registry. The owning module
// must outlive this view; MoonRuntime PinnedBinding/PinnedGeneration provide
// that lifetime boundary for loader consumers.
class RuntimeDescriptorRegistryView {
public:
    bool bind(const LunaRuntimeDescriptorRegistryV1* registry,
              std::string& error);
    explicit operator bool() const { return registry_ != nullptr; }

    const std::string& moduleId() const { return moduleId_; }
    uint64_t size() const;
    const LunaRuntimeDeclarationDescriptorV1* at(uint64_t index) const;
    const LunaRuntimeDeclarationDescriptorV1* find(
        const std::string& symbolId, const std::string& contractId,
        uint32_t declarationKind, uint32_t requiredFlags) const;

private:
    const LunaRuntimeDescriptorRegistryV1* registry_ = nullptr;
    std::string moduleId_;
};

} // namespace luna::runtime
