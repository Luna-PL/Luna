#include "runtime/Runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

constexpr size_t initialSize = 37;
constexpr size_t expandedSize = 113;
constexpr size_t reducedSize = 19;

unsigned char pattern(size_t index) {
    return static_cast<unsigned char>((index * 29 + 7) & 0xff);
}

bool hasAlignment(const void* pointer, size_t alignment) {
    return reinterpret_cast<uintptr_t>(pointer) % alignment == 0;
}

bool hasPattern(const void* pointer, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(pointer);
    for (size_t index = 0; index < size; ++index) {
        if (bytes[index] != pattern(index)) return false;
    }
    return true;
}

bool exercise(size_t alignment) {
    void* allocation = rt_alloc(initialSize, alignment);
    if (!allocation || !hasAlignment(allocation, alignment)) return false;
    auto* bytes = static_cast<unsigned char*>(allocation);
    for (size_t index = 0; index < initialSize; ++index)
        bytes[index] = pattern(index);

    void* expanded = rt_realloc(
        allocation, initialSize, expandedSize, alignment);
    if (!expanded) {
        rt_dealloc(allocation, initialSize, alignment);
        return false;
    }
    if (!hasAlignment(expanded, alignment) ||
        !hasPattern(expanded, initialSize)) {
        rt_dealloc(expanded, expandedSize, alignment);
        return false;
    }

    bytes = static_cast<unsigned char*>(expanded);
    for (size_t index = initialSize; index < expandedSize; ++index)
        bytes[index] = pattern(index);

    void* reduced = rt_realloc(
        expanded, expandedSize, reducedSize, alignment);
    if (!reduced) {
        rt_dealloc(expanded, expandedSize, alignment);
        return false;
    }
    if (!hasAlignment(reduced, alignment) ||
        !hasPattern(reduced, reducedSize)) {
        rt_dealloc(reduced, reducedSize, alignment);
        return false;
    }
    return rt_realloc(reduced, reducedSize, 0, alignment) == nullptr;
}

} // namespace

int main() {
    constexpr std::array<size_t, 6> alignments{
        1, 2, 8, alignof(std::max_align_t), 64, 256};
    for (const size_t alignment : alignments) {
        if (!exercise(alignment)) {
            std::cerr << "default allocation path failed for alignment "
                      << alignment << '\n';
            return 1;
        }
    }
    return 0;
}
