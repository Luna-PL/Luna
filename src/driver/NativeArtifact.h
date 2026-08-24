#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/NativeArtifactABI.h"

namespace luna::driver {

struct NativeExportSpec {
    uint32_t declarationKind = 0;
    uint32_t flags = 0;
    std::string symbolId;
    std::string contractId;
    std::string linkageName;
};

struct NativeProofSpec {
    std::string packageId;
    std::string packageVersion;
    std::string targetAbi;
    std::string compilerIdentity;
    std::vector<std::string> exportedDescriptors;
};

struct NativeProofInfo {
    std::string packageId;
    std::string packageVersion;
    std::string targetAbi;
    std::string compilerIdentity;
    std::array<uint8_t, LUNA_NATIVE_PROOF_DIGEST_SIZE> artifactDigest{};
    std::array<uint8_t, LUNA_NATIVE_PROOF_DIGEST_SIZE> exportDigest{};
    std::array<uint8_t, LUNA_NATIVE_PROOF_DIGEST_SIZE> dependencyDigest{};
};

class VerifiedNativeLibrary {
public:
    VerifiedNativeLibrary() = default;
    ~VerifiedNativeLibrary();
    VerifiedNativeLibrary(const VerifiedNativeLibrary&) = delete;
    VerifiedNativeLibrary& operator=(const VerifiedNativeLibrary&) = delete;
    VerifiedNativeLibrary(VerifiedNativeLibrary&& other) noexcept;
    VerifiedNativeLibrary& operator=(VerifiedNativeLibrary&& other) noexcept;

    const NativeProofInfo& proof() const { return proof_; }
    uint64_t exportCount() const;
    const LunaNativeExportDescriptorV1* exportAt(uint64_t index) const;
    const LunaNativeExportDescriptorV1* findExport(
        const std::string& symbolId, const std::string& contractId) const;
    explicit operator bool() const { return nativeHandle_ != nullptr; }

private:
    friend bool loadVerifiedNativeLibrary(
        const std::string&, const std::string&, VerifiedNativeLibrary&,
        std::string&, void (*)(void*), void*);
    void reset() noexcept;

    void* nativeHandle_ = nullptr;
    intptr_t stagingHandle_ = -1;
    std::string stagedPath_;
    std::string stagedDirectory_;
    const LunaNativeLibraryDescriptorV1* descriptor_ = nullptr;
    NativeProofInfo proof_;
};

bool makeNativeProofPlaceholder(const NativeProofSpec& spec,
                                std::vector<uint8_t>& record,
                                std::string& error);
bool sealNativeArtifact(const std::string& artifactPath,
                        const std::string& trustRecordPath,
                        NativeProofInfo& info, std::string& error);
bool verifyNativeArtifact(const std::string& artifactPath,
                          const std::string& trustStorePath,
                          NativeProofInfo& info, std::string& error);
// Captures artifactPath into an immutable/private staging image, verifies that
// exact image, then loads and validates its proof-bound typed registry. The
// optional hook runs after verification while staging remains immutable; it is
// intended for deterministic TOCTOU tests and receives no staging identity.
bool loadVerifiedNativeLibrary(
    const std::string& artifactPath, const std::string& trustStorePath,
    VerifiedNativeLibrary& library, std::string& error,
    void (*afterVerification)(void*) = nullptr,
    void* afterVerificationContext = nullptr);
std::string nativeDigestHex(
    const std::array<uint8_t, LUNA_NATIVE_PROOF_DIGEST_SIZE>& digest);
std::string canonicalNativeExport(const NativeExportSpec& descriptor);

} // namespace luna::driver
