#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr int kIterations = 20'000'000;
constexpr int kAllocations = 500'000;

std::uint32_t mix(std::uint32_t value) {
    return (value * 1'664'525u + 1'013'904'223u) & 2'147'483'647u;
}

std::uint32_t arithmetic() {
    std::uint32_t value = 1;
    for (int i = 0; i < kIterations; ++i) value = mix(value);
    return value & 255u;
}

std::uint32_t branchy() {
    std::int32_t value = 1;
    for (int i = 0; i < kIterations; ++i) {
        if ((value & 1) == 0) value /= 2;
        else value = (value * 3 + 1) % 1'000'003;
    }
    return static_cast<std::uint32_t>(value) & 255u;
}

inline std::uint32_t step(std::uint32_t value) {
    return (value * 5u + 7u) & 2'147'483'647u;
}

std::uint32_t calls() {
    std::uint32_t value = 1;
    for (int i = 0; i < kIterations; ++i) value = step(value);
    return value & 255u;
}

std::uint32_t safe_array_reference() {
    std::array<std::int32_t, 8> values{1, 2, 3, 4, 5, 6, 7, 8};
    for (int i = 0; i < kIterations; ++i) {
        const int index = i & 7;
        values[static_cast<std::size_t>(index)] += i & 31;
    }
    std::int32_t total = 0;
    for (const auto value : values) total += value;
    return static_cast<std::uint32_t>(total) & 255u;
}

std::uint32_t allocation() {
    std::int32_t checksum = 0;
    for (int i = 0; i < kAllocations; ++i) {
        auto* item = new std::int32_t(i);
        checksum = (checksum + *item) % 1'000'003;
        delete item;
    }
    return static_cast<std::uint32_t>(checksum) & 255u;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::uint32_t result = 0;
    if (std::strcmp(argv[1], "arithmetic") == 0) result = arithmetic();
    else if (std::strcmp(argv[1], "branch") == 0) result = branchy();
    else if (std::strcmp(argv[1], "calls") == 0) result = calls();
    else if (std::strcmp(argv[1], "array") == 0) result = safe_array_reference();
    else if (std::strcmp(argv[1], "allocation") == 0) result = allocation();
    else return 2;
    std::printf("%u\n", result);
    return 0;
}
