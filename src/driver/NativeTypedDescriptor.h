#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include "runtime/NativeArtifactABI.h"

namespace luna::driver {

// Descriptor ABI v2 fixes this UTF-8 row spelling and SHA-256 framing. A
// different spelling, digest algorithm, or entry profile requires a new query
// version; old v2 loaders reject unknown profiles instead of downgrading.
inline std::string canonicalNativeTypedExport(
    uint32_t kind, uint32_t flags, uint32_t entryAbi,
    const std::string& symbolId, const std::string& contractId,
    const std::string& linkageName) {
    return "LUNA_NATIVE_EXPORT_V2\n" + std::to_string(kind) + '\n' +
        std::to_string(flags) + '\n' + std::to_string(entryAbi) + '\n' +
        symbolId + '\n' + contractId + '\n' + linkageName;
}

inline std::array<uint8_t, LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V2>
digestNativeTypedExports(
    std::vector<std::string> rows) {
    // Sorted unique rows, little-endian u32 count, then u32 byte length and
    // exact row bytes per entry. Process-local pointer values are excluded.
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    std::vector<uint8_t> bytes;
    auto appendU32 = [&](uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    };
    appendU32(static_cast<uint32_t>(rows.size()));
    for (const auto& row : rows) {
        appendU32(static_cast<uint32_t>(row.size()));
        bytes.insert(bytes.end(), row.begin(), row.end());
    }
    llvm::SHA256 hash;
    hash.update(llvm::ArrayRef<uint8_t>(bytes));
    return hash.final();
}

} // namespace luna::driver
