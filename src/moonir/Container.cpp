#include "Container.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace moon {
namespace {

constexpr std::array<uint8_t, 8> Magic = {
    0x89, 0x4d, 0x4f, 0x4f, 0x4e, 0x0d, 0x0a, 0x1a};
constexpr uint32_t HeaderSize = 80;
constexpr uint32_t DirectoryEntrySize = 32;
constexpr uint32_t DigestOffset = 48;
constexpr uint32_t DigestSize = 32;

struct DirectoryEntry {
    uint32_t id = 0;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t decodedLength = 0;
};

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

bool addWouldOverflow(uint64_t left, uint64_t right) {
    return right > std::numeric_limits<uint64_t>::max() - left;
}

bool alignEight(uint64_t value, uint64_t& aligned) {
    if (addWouldOverflow(value, 7)) return false;
    aligned = (value + 7) & ~uint64_t{7};
    return true;
}

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] =
            static_cast<uint8_t>((value >> (index * 8)) & 0xffu);
}

void writeU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index)
        bytes[offset + index] =
            static_cast<uint8_t>((value >> (index * 8)) & 0xffu);
}

uint32_t readU32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index)
        value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8);
    return value;
}

uint64_t readU64(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
    return value;
}

bool isKnownRequiredSection(uint32_t id) {
    return id >= static_cast<uint32_t>(ContainerSectionId::Manifest) &&
           id <= static_cast<uint32_t>(ContainerSectionId::Sysmeta);
}

bool hasAllRequiredSections(const std::vector<ContainerSection>& sections) {
    for (uint32_t required =
             static_cast<uint32_t>(ContainerSectionId::Manifest);
         required <= static_cast<uint32_t>(ContainerSectionId::Sysmeta);
         ++required) {
        const auto found = std::find_if(
            sections.begin(), sections.end(),
            [required](const ContainerSection& section) {
                return section.id == required;
            });
        if (found == sections.end()) return false;
    }
    return true;
}

std::array<uint8_t, DigestSize> digestFor(
    const std::vector<uint8_t>& encoded) {
    llvm::SHA256 digest;
    digest.update(llvm::ArrayRef<uint8_t>(
        encoded.data(), DigestOffset));
    const std::array<uint8_t, DigestSize> zeroDigest{};
    digest.update(llvm::ArrayRef<uint8_t>(zeroDigest));
    digest.update(llvm::ArrayRef<uint8_t>(
        encoded.data() + DigestOffset + DigestSize,
        encoded.size() - DigestOffset - DigestSize));
    return digest.final();
}

bool zeroRange(
    const std::vector<uint8_t>& input, uint64_t begin, uint64_t end) {
    if (begin > end || end > input.size()) return false;
    for (uint64_t offset = begin; offset < end; ++offset)
        if (input[static_cast<size_t>(offset)] != 0) return false;
    return true;
}

} // namespace

bool ContainerWriter::encode(
    std::vector<ContainerSection> sections,
    std::vector<uint8_t>& output,
    std::string& error,
    const ContainerLimits& limits) {
    output.clear();
    error.clear();
    if (sections.size() > limits.maximumSections)
        return fail(error, "Moon Container has too many sections");

    std::sort(
        sections.begin(), sections.end(),
        [](const ContainerSection& left, const ContainerSection& right) {
            return left.id < right.id;
        });
    uint32_t previous = 0;
    for (const auto& section : sections) {
        if (section.id == 0)
            return fail(error, "Moon Container section ID zero is invalid");
        if (section.id == previous)
            return fail(error, "Moon Container contains a duplicate section ID");
        if (!isKnownRequiredSection(section.id) &&
            (section.id & OptionalSectionBit) == 0)
            return fail(error, "Moon Container writer received an unknown required section");
        previous = section.id;
    }
    if (!hasAllRequiredSections(sections))
        return fail(error, "Moon Container is missing a required section");

    const uint64_t directoryBytes =
        static_cast<uint64_t>(sections.size()) * DirectoryEntrySize;
    if (addWouldOverflow(HeaderSize, directoryBytes))
        return fail(error, "Moon Container directory size overflows");
    uint64_t cursor = 0;
    if (!alignEight(HeaderSize + directoryBytes, cursor))
        return fail(error, "Moon Container directory alignment overflows");

    std::vector<DirectoryEntry> directory;
    directory.reserve(sections.size());
    for (const auto& section : sections) {
        if (section.payload.size() > limits.maximumContainerBytes)
            return fail(error, "Moon Container section exceeds the byte limit");
        DirectoryEntry entry;
        entry.id = section.id;
        entry.offset = cursor;
        entry.length = section.payload.size();
        entry.decodedLength = entry.length;
        if (addWouldOverflow(cursor, entry.length))
            return fail(error, "Moon Container section range overflows");
        const uint64_t end = cursor + entry.length;
        if (!alignEight(end, cursor))
            return fail(error, "Moon Container section alignment overflows");
        directory.push_back(entry);
    }
    if (!directory.empty())
        cursor = directory.back().offset + directory.back().length;
    if (cursor > limits.maximumContainerBytes ||
        cursor > std::numeric_limits<size_t>::max())
        return fail(error, "Moon Container exceeds the byte limit");

    output.assign(static_cast<size_t>(cursor), uint8_t{0});
    std::copy(Magic.begin(), Magic.end(), output.begin());
    writeU32(output, 8, FormatMajor);
    writeU32(output, 12, FormatMinor);
    writeU32(output, 16, HeaderSize);
    writeU32(output, 20, 0);
    writeU32(output, 24, static_cast<uint32_t>(sections.size()));
    writeU32(output, 28, 0);
    writeU64(output, 32, HeaderSize);
    writeU64(output, 40, output.size());

    for (size_t index = 0; index < directory.size(); ++index) {
        const size_t base = HeaderSize + index * DirectoryEntrySize;
        const auto& entry = directory[index];
        writeU32(output, base, entry.id);
        writeU32(output, base + 4, entry.flags);
        writeU64(output, base + 8, entry.offset);
        writeU64(output, base + 16, entry.length);
        writeU64(output, base + 24, entry.decodedLength);
        std::copy(
            sections[index].payload.begin(), sections[index].payload.end(),
            output.begin() + static_cast<size_t>(entry.offset));
    }
    const auto digest = digestFor(output);
    std::copy(
        digest.begin(), digest.end(), output.begin() + DigestOffset);
    return true;
}

bool ContainerWriter::writeFile(
    const std::string& path,
    std::vector<ContainerSection> sections,
    std::string& error,
    const ContainerLimits& limits) {
    std::vector<uint8_t> encoded;
    if (!encode(std::move(sections), encoded, error, limits)) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return fail(error, "cannot open Moon Container output file");
    file.write(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<std::streamsize>(encoded.size()));
    if (!file) return fail(error, "cannot write Moon Container output file");
    return true;
}

bool ContainerReader::parse(
    const std::vector<uint8_t>& input,
    std::string& error,
    const ContainerLimits& limits) {
    mSections.clear();
    mFormatMajor = 0;
    mFormatMinor = 0;
    error.clear();
    if (input.size() > limits.maximumContainerBytes)
        return fail(error, "Moon Container exceeds the byte limit");
    if (input.size() < HeaderSize)
        return fail(error, "Moon Container header is truncated");
    if (!std::equal(Magic.begin(), Magic.end(), input.begin()))
        return fail(error, "Moon Container magic is invalid");

    const uint32_t decodedFormatMajor = readU32(input, 8);
    const uint32_t decodedFormatMinor = readU32(input, 12);
    if (decodedFormatMajor != FormatMajor || decodedFormatMinor != FormatMinor)
        return fail(error, "Moon Container format version is unsupported");
    if (readU32(input, 16) != HeaderSize)
        return fail(error, "Moon Container header size is invalid");
    if (readU32(input, 20) != 0)
        return fail(error, "Moon Container compression or header flags are unsupported");
    const uint32_t sectionCount = readU32(input, 24);
    if (sectionCount > limits.maximumSections)
        return fail(error, "Moon Container has too many sections");
    if (readU32(input, 28) != 0)
        return fail(error, "Moon Container reserved header bits are non-zero");
    const uint64_t directoryOffset = readU64(input, 32);
    const uint64_t encodedFileSize = readU64(input, 40);
    if (directoryOffset != HeaderSize)
        return fail(error, "Moon Container directory offset is non-canonical");
    if (encodedFileSize != input.size())
        return fail(error, "Moon Container file size does not match its header");

    const uint64_t directoryBytes =
        static_cast<uint64_t>(sectionCount) * DirectoryEntrySize;
    if (addWouldOverflow(directoryOffset, directoryBytes) ||
        directoryOffset + directoryBytes > input.size())
        return fail(error, "Moon Container directory is truncated");
    uint64_t minimumPayloadOffset = 0;
    if (!alignEight(directoryOffset + directoryBytes, minimumPayloadOffset))
        return fail(error, "Moon Container directory alignment overflows");

    std::vector<DirectoryEntry> directory;
    directory.reserve(sectionCount);
    uint32_t previousId = 0;
    uint64_t previousEnd = minimumPayloadOffset;
    for (uint32_t index = 0; index < sectionCount; ++index) {
        const size_t base = static_cast<size_t>(
            directoryOffset + static_cast<uint64_t>(index) *
                                  DirectoryEntrySize);
        DirectoryEntry entry;
        entry.id = readU32(input, base);
        entry.flags = readU32(input, base + 4);
        entry.offset = readU64(input, base + 8);
        entry.length = readU64(input, base + 16);
        entry.decodedLength = readU64(input, base + 24);
        if (entry.id == 0 || entry.id <= previousId)
            return fail(error, "Moon Container section IDs are duplicate or out of order");
        if (!isKnownRequiredSection(entry.id) &&
            (entry.id & OptionalSectionBit) == 0)
            return fail(error, "Moon Container contains an unknown required section");
        if (entry.flags != 0 || entry.decodedLength != entry.length)
            return fail(error, "Moon Container section compression is unsupported");
        if ((entry.offset & 7u) != 0 || entry.offset < minimumPayloadOffset)
            return fail(error, "Moon Container section offset is not canonically aligned");
        if (addWouldOverflow(entry.offset, entry.length) ||
            entry.offset + entry.length > input.size())
            return fail(error, "Moon Container section range is outside the file");
        if (entry.offset < previousEnd)
            return fail(error, "Moon Container sections overlap");
        if (!zeroRange(input, previousEnd, entry.offset))
            return fail(error, "Moon Container padding is non-zero");
        previousId = entry.id;
        previousEnd = entry.offset + entry.length;
        directory.push_back(entry);
    }
    if (previousEnd != input.size())
        return fail(error, "Moon Container has non-canonical trailing bytes");

    const auto expectedDigest = digestFor(input);
    if (!std::equal(
            expectedDigest.begin(), expectedDigest.end(),
            input.begin() + DigestOffset))
        return fail(error, "Moon Container SHA-256 digest does not match");

    std::vector<ContainerSection> decodedSections;
    decodedSections.reserve(directory.size());
    for (const auto& entry : directory) {
        ContainerSection section;
        section.id = entry.id;
        section.payload.assign(
            input.begin() + static_cast<size_t>(entry.offset),
            input.begin() + static_cast<size_t>(entry.offset + entry.length));
        decodedSections.push_back(std::move(section));
    }
    if (!hasAllRequiredSections(decodedSections))
        return fail(error, "Moon Container is missing a required section");
    mSections = std::move(decodedSections);
    mFormatMajor = decodedFormatMajor;
    mFormatMinor = decodedFormatMinor;
    return true;
}

bool ContainerReader::readFile(
    const std::string& path,
    std::string& error,
    const ContainerLimits& limits) {
    std::error_code filesystemError;
    const uintmax_t size = std::filesystem::file_size(path, filesystemError);
    if (filesystemError)
        return fail(error, "cannot inspect Moon Container input file");
    if (size > limits.maximumContainerBytes ||
        size > std::numeric_limits<size_t>::max())
        return fail(error, "Moon Container exceeds the byte limit");
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(error, "cannot open Moon Container input file");
    std::vector<uint8_t> input(static_cast<size_t>(size));
    file.read(
        reinterpret_cast<char*>(input.data()),
        static_cast<std::streamsize>(input.size()));
    if (!file && !input.empty())
        return fail(error, "cannot read Moon Container input file");
    return parse(input, error, limits);
}

const ContainerSection* ContainerReader::find(uint32_t id) const {
    const auto found = std::lower_bound(
        mSections.begin(), mSections.end(), id,
        [](const ContainerSection& section, uint32_t value) {
            return section.id < value;
        });
    return found != mSections.end() && found->id == id ? &*found : nullptr;
}

} // namespace moon
