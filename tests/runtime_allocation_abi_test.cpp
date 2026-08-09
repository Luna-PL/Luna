#include "runtime/Runtime.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace {

struct TestAllocator {
    bool failAllocate = false;
    bool failReallocate = false;
    size_t allocateCalls = 0;
    size_t reallocateCalls = 0;
    size_t deallocateCalls = 0;
};

int sharedDropCalls = 0;
int sharedDropSum = 0;

void dropSharedI32(void* storage) {
    ++sharedDropCalls;
    sharedDropSum += *static_cast<int32_t*>(storage);
}

void* allocate(void* context, size_t size, size_t) {
    auto& allocator = *static_cast<TestAllocator*>(context);
    ++allocator.allocateCalls;
    return allocator.failAllocate ? nullptr : std::malloc(size);
}

void* reallocate(void* context, void* pointer, size_t, size_t newSize,
                 size_t) {
    auto& allocator = *static_cast<TestAllocator*>(context);
    ++allocator.reallocateCalls;
    if (allocator.failReallocate) return nullptr;
    return std::realloc(pointer, newSize);
}

void deallocate(void* context, void* pointer, size_t, size_t) {
    auto& allocator = *static_cast<TestAllocator*>(context);
    ++allocator.deallocateCalls;
    std::free(pointer);
}

bool allocationError(const LunaAllocErrorV1& error, uint32_t kind,
                     size_t size, size_t alignment) {
    return error.abi_version == LUNA_RUNTIME_ABI_V1 &&
        error.struct_size == sizeof(LunaAllocErrorV1) &&
        error.kind == kind && error.reserved_zero == 0 &&
        error.requested_size == size && error.alignment == alignment;
}

} // namespace

int main() {
    TestAllocator state;
    const LunaAllocatorV1 allocator{
        LUNA_RUNTIME_ABI_V1, sizeof(LunaAllocatorV1), &state,
        allocate, reallocate, deallocate};
    const LunaHostServicesV1 services{
        LUNA_HOST_SERVICES_MAGIC_V1,
        LUNA_RUNTIME_ABI_V1,
        LUNA_HOST_SERVICES_V1_BASE_SIZE,
        0,
        LUNA_HOST_CAP_ALLOCATOR,
        &allocator,
        nullptr,
        nullptr,
        nullptr,
    };
    if (rt_install_host_services_v1(&services) != LUNA_RUNTIME_STATUS_OK) {
        std::cerr << "custom allocator host was rejected\n";
        return 1;
    }

    LunaAllocErrorV1 error{};
    size_t byteSize = 1;
    if (rt_checked_array_layout_v1(4, 8, 4, &byteSize, &error) !=
            LUNA_RUNTIME_STATUS_OK || byteSize != 32) {
        std::cerr << "valid array layout failed\n";
        return 1;
    }
    if (rt_checked_array_layout_v1(
            std::numeric_limits<size_t>::max(), 2, 8,
            &byteSize, &error) != LUNA_RUNTIME_STATUS_ALLOCATION_ERROR ||
        byteSize != 0 ||
        !allocationError(error, LUNA_ALLOC_ERROR_SIZE_OVERFLOW, 0, 8)) {
        std::cerr << "array layout overflow was not recoverable\n";
        return 1;
    }
    if (rt_checked_array_layout_v1(4, 8, 3, &byteSize, &error) !=
            LUNA_RUNTIME_STATUS_ALLOCATION_ERROR ||
        !allocationError(error, LUNA_ALLOC_ERROR_INVALID_ALIGNMENT, 0, 3)) {
        std::cerr << "invalid layout alignment was not recoverable\n";
        return 1;
    }

    void* memory = reinterpret_cast<void*>(1);
    if (rt_try_alloc_v1(0, 8, &memory, &error) != LUNA_RUNTIME_STATUS_OK ||
        memory != nullptr || state.allocateCalls != 0) {
        std::cerr << "zero-size allocation was not allocation-free\n";
        return 1;
    }
    state.failAllocate = true;
    if (rt_try_alloc_v1(32, 8, &memory, &error) !=
            LUNA_RUNTIME_STATUS_ALLOCATION_ERROR || memory != nullptr ||
        !allocationError(error, LUNA_ALLOC_ERROR_OUT_OF_MEMORY, 32, 8)) {
        std::cerr << "allocation failure was not represented\n";
        return 1;
    }
    state.failAllocate = false;
    if (rt_try_alloc_v1(32, 8, &memory, &error) != LUNA_RUNTIME_STATUS_OK ||
        !memory) {
        std::cerr << "fallible allocation did not succeed\n";
        return 1;
    }
    std::memset(memory, 0x5a, 32);

    const size_t callsBeforeInvalid = state.reallocateCalls;
    void* replacement = nullptr;
    if (rt_try_realloc_v1(memory, 32, 64, 3, &replacement, &error) !=
            LUNA_RUNTIME_STATUS_ALLOCATION_ERROR || replacement != memory ||
        state.reallocateCalls != callsBeforeInvalid ||
        !allocationError(error, LUNA_ALLOC_ERROR_INVALID_ALIGNMENT, 64, 3)) {
        std::cerr << "invalid realloc alignment changed the allocation\n";
        return 1;
    }
    replacement = nullptr;
    if (rt_try_realloc_v1(memory, 32, 0, 8, &replacement, &error) !=
            LUNA_RUNTIME_STATUS_INVALID_ARGUMENT || replacement != memory ||
        state.reallocateCalls != callsBeforeInvalid) {
        std::cerr << "zero-size realloc consumed the allocation\n";
        return 1;
    }

    state.failReallocate = true;
    replacement = nullptr;
    if (rt_try_realloc_v1(memory, 32, 64, 8, &replacement, &error) !=
            LUNA_RUNTIME_STATUS_ALLOCATION_ERROR || replacement != memory ||
        !allocationError(error, LUNA_ALLOC_ERROR_OUT_OF_MEMORY, 64, 8)) {
        std::cerr << "failed realloc did not preserve the original pointer\n";
        return 1;
    }
    const auto* bytes = static_cast<const unsigned char*>(memory);
    for (size_t index = 0; index < 32; ++index) {
        if (bytes[index] != 0x5a) {
            std::cerr << "failed realloc changed original bytes\n";
            return 1;
        }
    }

    state.failReallocate = false;
    if (rt_try_realloc_v1(memory, 32, 64, 8, &replacement, &error) !=
            LUNA_RUNTIME_STATUS_OK || !replacement) {
        std::cerr << "fallible realloc did not succeed\n";
        return 1;
    }
    memory = replacement;
    bytes = static_cast<const unsigned char*>(memory);
    for (size_t index = 0; index < 32; ++index) {
        if (bytes[index] != 0x5a) {
            std::cerr << "successful realloc did not retain existing bytes\n";
            return 1;
        }
    }

    void* second = nullptr;
    if (rt_try_realloc_v1(nullptr, 0, 16, 8, &second, &error) !=
            LUNA_RUNTIME_STATUS_OK || !second ||
        rt_try_realloc_v1(nullptr, 4, 16, 8, &replacement, &error) !=
            LUNA_RUNTIME_STATUS_INVALID_ARGUMENT) {
        std::cerr << "null realloc allocation rules are inconsistent\n";
        return 1;
    }
    rt_dealloc(second, 16, 8);
    rt_dealloc(memory, 64, 8);
    if (state.deallocateCalls != 2) {
        std::cerr << "successful allocations were not released exactly once\n";
        return 1;
    }

    void* rc = rt_rc_allocate_v1(
        sizeof(int32_t), alignof(int32_t), dropSharedI32);
    if (!rc || reinterpret_cast<uintptr_t>(rc) % alignof(int32_t) != 0) {
        std::cerr << "Rc shared cell allocation lost its payload alignment\n";
        return 1;
    }
    *static_cast<int32_t*>(rc) = 17;
    rt_rc_retain_v1(rc);
    rt_rc_release_v1(rc);
    if (sharedDropCalls != 0 || state.deallocateCalls != 2) {
        std::cerr << "Rc released a retained shared cell too early\n";
        return 1;
    }
    rt_rc_release_v1(rc);
    if (sharedDropCalls != 1 || sharedDropSum != 17 ||
        state.deallocateCalls != 3) {
        std::cerr << "Rc final release did not drop and deallocate exactly once\n";
        return 1;
    }

    void* arc = rt_arc_allocate_v1(
        sizeof(int32_t), alignof(int32_t), dropSharedI32);
    if (!arc || reinterpret_cast<uintptr_t>(arc) % alignof(int32_t) != 0) {
        std::cerr << "Arc shared cell allocation lost its payload alignment\n";
        return 1;
    }
    *static_cast<int32_t*>(arc) = 25;
    rt_arc_retain_v1(arc);
    rt_arc_release_v1(arc);
    if (sharedDropCalls != 1 || state.deallocateCalls != 3) {
        std::cerr << "Arc released a retained shared cell too early\n";
        return 1;
    }
    rt_arc_release_v1(arc);
    if (sharedDropCalls != 2 || sharedDropSum != 42 ||
        state.deallocateCalls != 4) {
        std::cerr << "Arc final release did not drop and deallocate exactly once\n";
        return 1;
    }

    const size_t callsBeforeInvalidShared = state.allocateCalls;
    if (rt_rc_allocate_v1(-1, 8, dropSharedI32) != nullptr ||
        rt_arc_allocate_v1(4, 3, dropSharedI32) != nullptr ||
        state.allocateCalls != callsBeforeInvalidShared) {
        std::cerr << "invalid shared-cell layout reached the host allocator\n";
        return 1;
    }
    return 0;
}
