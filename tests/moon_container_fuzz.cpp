#include "moonir/Container.h"
#include "moonir/ContainerModel.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::array<uint8_t, 8> Magic = {
    0x89, 0x4d, 0x4f, 0x4e, 0x0d, 0x0a, 0x1a};
constexpr size_t HeaderSize = 80;
constexpr size_t DigestOffset = 48;
constexpr size_t DigestSize = 32;
constexpr size_t DirectoryEntrySize = 32;

[[noreturn]] void invariantFailure() {
    std::abort();
}

moon::ContainerLimits fuzzLimits() {
    moon::ContainerLimits limits;
    limits.maximumContainerBytes = 1u << 20;
    limits.maximumSections = 64;
    limits.maximumStringBytes = 64u << 10;
    limits.maximumTableRows = 4096;
    limits.maximumNestingDepth = 64;
    return limits;
}

void require(bool condition) {
    if (!condition) invariantFailure();
}

uint32_t readU32(const uint8_t* data) {
    uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
        value |= static_cast<uint32_t>(data[index]) << (index * 8);
    return value;
}

uint64_t readU64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= static_cast<uint64_t>(data[index]) << (index * 8);
    return value;
}

void authenticate(uint8_t* data, size_t size) {
    if (size < HeaderSize) return;
    llvm::SHA256 digest;
    digest.update(llvm::ArrayRef<uint8_t>(data, DigestOffset));
    const std::array<uint8_t, DigestSize> zeroDigest{};
    digest.update(llvm::ArrayRef<uint8_t>(zeroDigest));
    digest.update(llvm::ArrayRef<uint8_t>(
        data + DigestOffset + DigestSize,
        size - DigestOffset - DigestSize));
    const auto result = digest.final();
    std::copy(result.begin(), result.end(), data + DigestOffset);
}

} // namespace

extern "C" size_t LLVMFuzzerMutate(
    uint8_t* data, size_t size, size_t maximumSize);

extern "C" size_t LLVMFuzzerCustomMutator(
    uint8_t* data, size_t size, size_t maximumSize, unsigned seed) {
    if (size < HeaderSize ||
        !std::equal(Magic.begin(), Magic.end(), data))
        return LLVMFuzzerMutate(data, size, maximumSize);

    const uint32_t sectionCount = readU32(data + 24);
    if (sectionCount == 0 || sectionCount > 64 ||
        sectionCount > (size - HeaderSize) / DirectoryEntrySize)
        return LLVMFuzzerMutate(data, size, maximumSize);

    // Preserve length and framing for most mutations so SHA-256 does not
    // prevent coverage-guided exploration of the model decoders. Raw generic
    // mutations remain one quarter of the schedule and continue exercising
    // magic, truncation, length, and integrity rejection.
    if ((seed & 3u) == 0)
        return LLVMFuzzerMutate(data, size, maximumSize);

    if ((seed & 3u) == 1) {
        const size_t row = (seed >> 2) % sectionCount;
        const size_t fieldByte = (seed >> 10) % DirectoryEntrySize;
        data[HeaderSize + row * DirectoryEntrySize + fieldByte] ^=
            static_cast<uint8_t>(1u << ((seed >> 18) & 7u));
        authenticate(data, size);
        return size;
    }

    const size_t firstRow = (seed >> 2) % sectionCount;
    for (size_t step = 0; step < sectionCount; ++step) {
        const size_t row = (firstRow + step) % sectionCount;
        const uint8_t* entry = data + HeaderSize + row * DirectoryEntrySize;
        const uint64_t offset = readU64(entry + 8);
        const uint64_t length = readU64(entry + 16);
        if (length == 0 || offset > size || length > size - offset) continue;
        const size_t byte = static_cast<size_t>(
            offset + ((seed >> 10) % length));
        if ((seed & 3u) == 2)
            data[byte] ^= static_cast<uint8_t>(
                1u << ((seed >> 18) & 7u));
        else
            data[byte] = static_cast<uint8_t>(seed >> 18);
        authenticate(data, size);
        return size;
    }
    return LLVMFuzzerMutate(data, size, maximumSize);
}

extern "C" int LLVMFuzzerTestOneInput(
    const uint8_t* data, size_t size) {
    const auto limits = fuzzLimits();
    if (size > limits.maximumContainerBytes) return 0;

    std::vector<uint8_t> input;
    if (size != 0) input.assign(data, data + size);

    std::string error;
    moon::ContainerReader reader;
    const bool framingAccepted = reader.parse(input, error, limits);
    if (!framingAccepted) {
        require(!error.empty());
        require(reader.sections().empty());
        require(reader.formatMajor() == 0 && reader.formatMinor() == 0);
        return 0;
    }

    require(error.empty());
    std::vector<uint8_t> canonicalFraming;
    require(moon::ContainerWriter::encode(
        reader.sections(), canonicalFraming, error, limits));
    require(canonicalFraming == input);

    moon::ContainerManifest manifest;
    manifest.packageId = "fuzz.sentinel.manifest";
    moon::Module module;
    module.name = "fuzz.sentinel.module";
    module.sourceModules.push_back("sentinel");
    const bool modelAccepted = moon::ContainerModelCodec::decodeContainer(
        input, manifest, module, error, limits);
    if (!modelAccepted) {
        require(!error.empty());
        require(manifest.packageId == "fuzz.sentinel.manifest");
        require(module.name == "fuzz.sentinel.module");
        require(module.sourceModules.size() == 1 &&
                module.sourceModules.front() == "sentinel");
        require(module.typeTable.empty());
        require(module.declarationTable.empty());
        require(module.declarations.empty());
        return 0;
    }

    require(error.empty());
    std::vector<uint8_t> canonicalModel;
    require(moon::ContainerModelCodec::encodeContainer(
        manifest, module, canonicalModel, error, limits));
    moon::ContainerReader canonicalReader;
    require(canonicalReader.parse(canonicalModel, error, limits));
    for (uint32_t id = static_cast<uint32_t>(
             moon::ContainerSectionId::Manifest);
         id <= static_cast<uint32_t>(moon::ContainerSectionId::Sysmeta);
         ++id) {
        const auto* original = reader.find(id);
        const auto* reencoded = canonicalReader.find(id);
        require(original && reencoded && original->payload == reencoded->payload);
    }
    return 0;
}
