#include "runtime/Runtime.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

struct TestHost {
    size_t allocated_size = 0;
    size_t allocated_alignment = 0;
    size_t reallocated_old_size = 0;
    size_t reallocated_new_size = 0;
    size_t deallocated_size = 0;
    size_t deallocated_alignment = 0;
    std::string output;
};

void* allocate(void* context, size_t size, size_t alignment) {
    auto& host = *static_cast<TestHost*>(context);
    host.allocated_size = size;
    host.allocated_alignment = alignment;
    return std::malloc(size == 0 ? 1 : size);
}

void* reallocate(void* context, void* pointer, size_t oldSize,
                 size_t newSize, size_t) {
    auto& host = *static_cast<TestHost*>(context);
    host.reallocated_old_size = oldSize;
    host.reallocated_new_size = newSize;
    return std::realloc(pointer, newSize);
}

void deallocate(void* context, void* pointer, size_t size, size_t alignment) {
    auto& host = *static_cast<TestHost*>(context);
    host.deallocated_size = size;
    host.deallocated_alignment = alignment;
    std::free(pointer);
}

int writeConsole(void* context, uint32_t stream, const char* bytes, size_t byteCount) {
    if (stream != LUNA_CONSOLE_STDOUT) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    static_cast<TestHost*>(context)->output.append(bytes, byteCount);
    return LUNA_RUNTIME_STATUS_OK;
}

int flushConsole(void*, uint32_t) {
    return LUNA_RUNTIME_STATUS_OK;
}

} // namespace

int main() {
    TestHost host;
    const LunaAllocatorV1 allocator{
        LUNA_RUNTIME_ABI_V1, sizeof(LunaAllocatorV1), &host,
        allocate, reallocate, deallocate};
    const LunaConsoleV1 console{
        LUNA_RUNTIME_ABI_V1, sizeof(LunaConsoleV1), &host,
        writeConsole, flushConsole};
    const LunaHostServicesV1 services{
        LUNA_HOST_SERVICES_MAGIC_V1,
        LUNA_RUNTIME_ABI_V1,
        sizeof(LunaHostServicesV1),
        0,
        LUNA_HOST_CAP_ALLOCATOR | LUNA_HOST_CAP_CONSOLE,
        &allocator,
        &console,
        nullptr,
    };

    LunaHostServicesV1 truncated = services;
    truncated.struct_size = sizeof(LunaHostServicesV1) - 1;
    if (rt_install_host_services_v1(&truncated) !=
        LUNA_RUNTIME_STATUS_INVALID_ARGUMENT) {
        std::cerr << "truncated host-service descriptor was accepted\n";
        return 1;
    }

    if (rt_install_host_services_v1(&services) != LUNA_RUNTIME_STATUS_OK) {
        std::cerr << "could not install host services before activation\n";
        return 1;
    }

    void* memory = rt_alloc(24, 8);
    if (!memory || host.allocated_size != 24 || host.allocated_alignment != 8) {
        std::cerr << "allocation layout was not forwarded through Runtime ABI v1\n";
        return 1;
    }
    std::memset(memory, 0x2a, 24);
    memory = rt_realloc(memory, 24, 40, 8);
    if (!memory || host.reallocated_old_size != 24 || host.reallocated_new_size != 40) {
        std::cerr << "reallocation layout was not forwarded through Runtime ABI v1\n";
        return 1;
    }
    rt_dealloc(memory, 40, 8);
    if (host.deallocated_size != 40 || host.deallocated_alignment != 8) {
        std::cerr << "deallocation layout was not forwarded through Runtime ABI v1\n";
        return 1;
    }

    rt_print_i32(41);
    rt_print_cstr("runtime ABI");
    if (host.output != "41\nruntime ABI\n") {
        std::cerr << "language console output bypassed the installed host service\n";
        return 1;
    }

    const auto* active = rt_host_services_v1();
    if (active != &services || active->executable_memory != nullptr ||
        (active->capabilities & LUNA_HOST_CAP_EXECUTABLE_MEMORY) != 0) {
        std::cerr << "active host-service descriptor is inconsistent\n";
        return 1;
    }
    if (rt_install_host_services_v1(&services) != LUNA_RUNTIME_STATUS_ALREADY_ACTIVE) {
        std::cerr << "allocator replacement was accepted after runtime activation\n";
        return 1;
    }
    return 0;
}
