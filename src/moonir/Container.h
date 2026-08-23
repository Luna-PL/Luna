#pragma once

#include "MoonIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace moon {

enum class ContainerSectionId : uint32_t {
    Manifest = 1,
    Type = 2,
    Symbol = 3,
    Contract = 4,
    Code = 5,
    Imports = 6,
    Exports = 7,
    Sysmeta = 8,
};

inline constexpr uint32_t OptionalSectionBit = 0x80000000u;

struct ContainerSection {
    uint32_t id = 0;
    std::vector<uint8_t> payload;
};

struct ContainerLimits {
    uint64_t maximumContainerBytes = 1ull << 30;
    uint32_t maximumSections = 64;
    uint32_t maximumStringBytes = 16u << 20;
    uint32_t maximumTableRows = 1u << 24;
    uint32_t maximumNestingDepth = 256;
};

class ContainerWriter {
public:
    static bool encode(
        std::vector<ContainerSection> sections,
        std::vector<uint8_t>& output,
        std::string& error,
        const ContainerLimits& limits = {});

    static bool writeFile(
        const std::string& path,
        std::vector<ContainerSection> sections,
        std::string& error,
        const ContainerLimits& limits = {});
};

class ContainerReader {
public:
    bool parse(
        const std::vector<uint8_t>& input,
        std::string& error,
        const ContainerLimits& limits = {});

    bool readFile(
        const std::string& path,
        std::string& error,
        const ContainerLimits& limits = {});

    const std::vector<ContainerSection>& sections() const {
        return mSections;
    }

    const ContainerSection* find(uint32_t id) const;
    uint32_t formatMajor() const { return mFormatMajor; }
    uint32_t formatMinor() const { return mFormatMinor; }

private:
    std::vector<ContainerSection> mSections;
    uint32_t mFormatMajor = 0;
    uint32_t mFormatMinor = 0;
};

} // namespace moon
