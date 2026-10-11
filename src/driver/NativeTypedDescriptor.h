#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include "runtime/NativeArtifactABI.h"

namespace luna::driver {

inline bool validNativeDescriptorUtf8(std::string_view value) {
    size_t index = 0;
    while (index < value.size()) {
        const unsigned char first =
            static_cast<unsigned char>(value[index++]);
        if (first <= 0x7f) continue;
        uint32_t scalar = 0;
        size_t remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            scalar = first & 0x1f;
            remaining = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            scalar = first & 0x0f;
            remaining = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            scalar = first & 0x07;
            remaining = 3;
        } else {
            return false;
        }
        if (remaining > value.size() - index) return false;
        for (size_t offset = 0; offset < remaining; ++offset) {
            const unsigned char next =
                static_cast<unsigned char>(value[index++]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if ((remaining == 2 && scalar < 0x800) ||
            (remaining == 3 && scalar < 0x10000) ||
            (scalar >= 0xd800 && scalar <= 0xdfff) || scalar > 0x10ffff)
            return false;
    }
    return true;
}

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

// V3 uses the same length-framed SHA-256 list algorithm as V2 but a distinct
// row domain. These ten UTF-8 identifiers are ordered exactly as in the V3
// C record, from symbol_id through error_drop_contract_id.
inline std::string canonicalNativeOwnedResultExportV3(
    uint32_t kind, uint32_t flags, uint32_t entryAbi, uint32_t refMode,
    uint32_t resultMode, uint32_t statusDomain, uint64_t errorSize,
    uint64_t errorAlignment, const std::array<std::string, 10>& identifiers) {
    std::string row = "LUNA_NATIVE_EXPORT_V3\n" +
        std::to_string(kind) + '\n' + std::to_string(flags) + '\n' +
        std::to_string(entryAbi) + '\n' + std::to_string(refMode) + '\n' +
        std::to_string(resultMode) + '\n' + std::to_string(statusDomain) + '\n' +
        std::to_string(errorSize) + '\n' + std::to_string(errorAlignment);
    for (const auto& identifier : identifiers)
        row += '\n' + identifier;
    return row;
}

// V4 binds the borrowed Ref/unit source and wrapper identities without
// serializing process-local pointers. The ten identifiers follow the C row.
inline std::string canonicalNativeRefUnitExportV4(
    uint32_t kind, uint32_t flags, uint32_t entryAbi, uint32_t refMode,
    uint32_t resultMode, uint32_t statusDomain,
    const std::array<std::string, 10>& identifiers) {
    std::string row = "LUNA_NATIVE_EXPORT_V4\n" +
        std::to_string(kind) + '\n' + std::to_string(flags) + '\n' +
        std::to_string(entryAbi) + '\n' + std::to_string(refMode) + '\n' +
        std::to_string(resultMode) + '\n' + std::to_string(statusDomain);
    for (const auto& identifier : identifiers)
        row += '\n' + identifier;
    return row;
}

} // namespace luna::driver
