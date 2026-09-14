#pragma once

#include "driver/NativeArtifact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace luna::driver::native_artifact_detail {

using Digest = std::array<uint8_t, LUNA_NATIVE_PROOF_DIGEST_SIZE>;

inline constexpr size_t MaxNativeArtifactBytes =
    512u * 1024u * 1024u;
inline constexpr uint64_t MaxNativeExportCount = 1u << 20;
inline constexpr size_t MaxNativeDescriptorString = 4096;

Digest digestList(std::vector<std::string> values);
bool readFile(
    const std::string& path, std::vector<uint8_t>& bytes,
    std::string& error);

} // namespace luna::driver::native_artifact_detail
