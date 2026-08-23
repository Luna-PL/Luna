#include "moonir/Container.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

std::vector<moon::ContainerSection> fixtureSections(bool optional = false) {
    std::vector<moon::ContainerSection> result;
    for (uint32_t id =
             static_cast<uint32_t>(moon::ContainerSectionId::Manifest);
         id <= static_cast<uint32_t>(moon::ContainerSectionId::Sysmeta);
         ++id) {
        moon::ContainerSection section;
        section.id = id;
        section.payload = {
            static_cast<uint8_t>(id),
            static_cast<uint8_t>(id * 3),
            static_cast<uint8_t>(id * 7)};
        result.push_back(std::move(section));
    }
    if (optional)
        result.push_back({moon::OptionalSectionBit | 17u, {1, 2, 3, 4}});
    std::reverse(result.begin(), result.end());
    return result;
}

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] =
            static_cast<uint8_t>((value >> (index * 8)) & 0xffu);
}

} // namespace

int main(int argc, char* argv[]) {
    std::string error;
    std::vector<uint8_t> first;
    if (!moon::ContainerWriter::encode(
            fixtureSections(true), first, error)) {
        std::cerr << error << '\n';
        return fail("deterministic Moon Container fixture did not encode");
    }
    std::vector<uint8_t> second;
    if (!moon::ContainerWriter::encode(
            fixtureSections(true), second, error) || first != second)
        return fail("Moon Container encoding is not deterministic");

    moon::ContainerReader reader;
    if (!reader.parse(first, error) || reader.sections().size() != 9 ||
        !reader.find(static_cast<uint32_t>(moon::ContainerSectionId::Code)) ||
        !reader.find(moon::OptionalSectionBit | 17u)) {
        std::cerr << error << '\n';
        return fail("Moon Container reader did not recover canonical sections");
    }

    auto malformed = first;
    malformed[0] ^= 1;
    if (reader.parse(malformed, error) ||
        error.find("magic") == std::string::npos)
        return fail("Moon Container reader accepted a corrupted magic");

    malformed = first;
    malformed.back() ^= 1;
    if (reader.parse(malformed, error) ||
        error.find("digest") == std::string::npos)
        return fail("Moon Container reader accepted corrupted section data");

    malformed = first;
    writeU32(malformed, 80 + 32, 1);
    if (reader.parse(malformed, error) ||
        error.find("duplicate or out of order") == std::string::npos)
        return fail("Moon Container reader accepted duplicate section IDs");

    malformed = first;
    writeU32(malformed, 80 + 4, 1);
    if (reader.parse(malformed, error) ||
        error.find("compression") == std::string::npos)
        return fail("Moon Container reader accepted compression flags");

    auto missing = fixtureSections();
    missing.pop_back();
    std::vector<uint8_t> rejected;
    if (moon::ContainerWriter::encode(
            std::move(missing), rejected, error) ||
        error.find("required section") == std::string::npos)
        return fail("Moon Container writer accepted a missing required section");

    if (!reader.parse(first, error))
        return fail("Moon Container reader could not restore its valid state");
    malformed = first;
    malformed[0] ^= 1;
    if (reader.parse(malformed, error) || !reader.sections().empty() ||
        reader.formatMajor() != 0 || reader.formatMinor() != 0)
        return fail("Moon Container reader published state after failure");

    auto duplicate = fixtureSections();
    duplicate.push_back(duplicate.front());
    if (moon::ContainerWriter::encode(
            std::move(duplicate), rejected, error) ||
        error.find("duplicate") == std::string::npos)
        return fail("Moon Container writer accepted a duplicate section");

    moon::ContainerLimits tiny;
    tiny.maximumContainerBytes = first.size() - 1;
    if (reader.parse(first, error, tiny) ||
        error.find("byte limit") == std::string::npos)
        return fail("Moon Container reader ignored its resource limit");

    if (argc == 2) {
        if (!moon::ContainerWriter::writeFile(
                argv[1], fixtureSections(true), error)) {
            std::cerr << error << '\n';
            return fail("Moon Container file writer failed");
        }
        moon::ContainerReader fileReader;
        if (!fileReader.readFile(argv[1], error) ||
            fileReader.sections().size() != 9) {
            std::cerr << error << '\n';
            return fail("Moon Container file reader failed");
        }
    }
    return 0;
}
