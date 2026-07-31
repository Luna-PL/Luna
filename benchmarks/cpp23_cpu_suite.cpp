#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Defined in a separate translation unit so an ordinary non-LTO benchmark
// build cannot fold the allocation/deallocation pair into scalar arithmetic.
extern "C" void* luna_benchmark_allocate(std::size_t size);
extern "C" void luna_benchmark_deallocate(void* pointer);

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
        auto* item = static_cast<std::int32_t*>(
            luna_benchmark_allocate(sizeof(std::int32_t)));
        *item = i;
        checksum = (checksum + i) % 1'000'003;
        luna_benchmark_deallocate(item);
    }
    return static_cast<std::uint32_t>(checksum) & 255u;
}

std::uint32_t bitmix() {
    std::uint32_t value = 305'419'896u;
    for (int i = 0; i < kIterations; ++i) {
        value = (value ^ (value << 13)) & 2'147'483'647u;
        value ^= value >> 17;
        value ^= value << 5;
        value &= 2'147'483'647u;
    }
    return value & 255u;
}

std::uint32_t reduction() {
    std::uint32_t first = 1;
    std::uint32_t second = 3;
    std::uint32_t third = 5;
    std::uint32_t fourth = 7;
    for (int i = 0; i < 10'000'000; ++i) {
        const auto value = static_cast<std::uint32_t>(i);
        first = (first * 33u + (value & 255u)) & 2'147'483'647u;
        second = (second * 65u + ((value * 3u) & 255u)) & 2'147'483'647u;
        third = (third * 129u + ((value * 5u) & 255u)) & 2'147'483'647u;
        fourth = (fourth * 257u + ((value * 7u) & 255u)) & 2'147'483'647u;
    }
    return (first + second + third + fourth) & 255u;
}

std::uint32_t array_scan() {
    std::array<std::int32_t, 64> values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::int32_t>(i + 1);
    }
    std::int32_t state = 1;
    for (int i = 0; i < kIterations; ++i) {
        const auto index = static_cast<std::size_t>(i & 63);
        const auto next = (static_cast<std::uint32_t>(values[index]) +
                           static_cast<std::uint32_t>(state) +
                           (static_cast<std::uint32_t>(i) & 31u)) &
                          2'147'483'647u;
        values[index] = static_cast<std::int32_t>(next);
        state = static_cast<std::int32_t>(
            (static_cast<std::uint32_t>(state) + next) & 2'147'483'647u);
    }
    return static_cast<std::uint32_t>(state) & 255u;
}

std::uint32_t nested() {
    std::uint32_t state = 1;
    for (int row = 0; row < 2'500; ++row) {
        for (int column = 0; column < 4'000; ++column) {
            state = (state * 33u + static_cast<std::uint32_t>(row) +
                     static_cast<std::uint32_t>(column)) &
                    2'147'483'647u;
        }
    }
    return state & 255u;
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
    else if (std::strcmp(argv[1], "bitmix") == 0) result = bitmix();
    else if (std::strcmp(argv[1], "reduction") == 0) result = reduction();
    else if (std::strcmp(argv[1], "array-scan") == 0) result = array_scan();
    else if (std::strcmp(argv[1], "nested") == 0) result = nested();
    else return 2;
    std::printf("%u\n", result);
    return 0;
}
