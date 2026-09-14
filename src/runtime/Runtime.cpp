#include "Runtime.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using AtomicSharedCounter = std::atomic<uint64_t>;

union SharedCounter {
    uint64_t rc;
    AtomicSharedCounter arc;

    SharedCounter() : rc(0) {}
    ~SharedCounter() {}
};

struct SharedAllocationHeader {
    void* allocation = nullptr;
    size_t allocationSize = 0;
    size_t allocationAlignment = 0;
    SharedCounter count;
    LunaDropCallbackV1 drop = nullptr;
};

SharedAllocationHeader* sharedHeader(void* pointer) {
    return reinterpret_cast<SharedAllocationHeader*>(
        static_cast<unsigned char*>(pointer) - sizeof(SharedAllocationHeader));
}

bool isValidAlignment(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

void* defaultAllocate(void*, size_t size, size_t alignment) {
    if (!isValidAlignment(alignment)) return nullptr;
    const size_t actualSize = size == 0 ? 1 : size;
    if (alignment <= alignof(std::max_align_t)) return std::malloc(actualSize);
#ifdef _WIN32
    return _aligned_malloc(actualSize, alignment);
#else
    void* allocation = nullptr;
    return posix_memalign(&allocation, alignment, actualSize) == 0
        ? allocation : nullptr;
#endif
}

void defaultDeallocate(void*, void* pointer, size_t, size_t alignment) {
    if (!pointer) return;
    if (alignment <= alignof(std::max_align_t)) {
        std::free(pointer);
        return;
    }
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void* defaultReallocate(void*, void* pointer, size_t oldSize,
                        size_t newSize, size_t alignment) {
    if (!isValidAlignment(alignment)) return nullptr;
    if (!pointer) return defaultAllocate(nullptr, newSize, alignment);
    if (alignment <= alignof(std::max_align_t)) {
        if (newSize == 0) {
            std::free(pointer);
            return nullptr;
        }
        return std::realloc(pointer, newSize);
    }
    if (newSize == 0) {
        defaultDeallocate(nullptr, pointer, oldSize, alignment);
        return nullptr;
    }
    void* replacement = defaultAllocate(nullptr, newSize, alignment);
    if (!replacement) return nullptr;
    std::memcpy(replacement, pointer, std::min(oldSize, newSize));
    defaultDeallocate(nullptr, pointer, oldSize, alignment);
    return replacement;
}

int defaultConsoleWrite(void*, uint32_t stream, const char* bytes, size_t byteCount) {
    if (!bytes && byteCount != 0) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    if (stream != LUNA_CONSOLE_STDOUT && stream != LUNA_CONSOLE_STDERR)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    FILE* output = stream == LUNA_CONSOLE_STDERR ? stderr : stdout;
    return std::fwrite(bytes, 1, byteCount, output) == byteCount
        ? LUNA_RUNTIME_STATUS_OK : LUNA_RUNTIME_STATUS_UNSUPPORTED_OPERATION;
}

int defaultConsoleFlush(void*, uint32_t stream) {
    if (stream != LUNA_CONSOLE_STDOUT && stream != LUNA_CONSOLE_STDERR)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    FILE* output = stream == LUNA_CONSOLE_STDERR ? stderr : stdout;
    return std::fflush(output) == 0
        ? LUNA_RUNTIME_STATUS_OK : LUNA_RUNTIME_STATUS_UNSUPPORTED_OPERATION;
}

const LunaAllocatorV1 defaultAllocator{
    LUNA_RUNTIME_ABI_V1,
    sizeof(LunaAllocatorV1),
    nullptr,
    defaultAllocate,
    defaultReallocate,
    defaultDeallocate,
};

const LunaConsoleV1 defaultConsole{
    LUNA_RUNTIME_ABI_V1,
    sizeof(LunaConsoleV1),
    nullptr,
    defaultConsoleWrite,
    defaultConsoleFlush,
    nullptr,
};

const LunaHostServicesV1 defaultHostServices{
    LUNA_HOST_SERVICES_MAGIC_V1,
    LUNA_RUNTIME_ABI_V1,
    sizeof(LunaHostServicesV1),
    0,
    LUNA_HOST_CAP_ALLOCATOR | LUNA_HOST_CAP_CONSOLE,
    &defaultAllocator,
    &defaultConsole,
    nullptr,
    nullptr,
};

std::atomic<const LunaHostServicesV1*> installedHostServices{&defaultHostServices};
// 0: configurable, -1: an installation is in progress, 1: services are live.
std::atomic<int> hostServicesPhase{0};

bool validAllocator(const LunaAllocatorV1* allocator) {
    return allocator && allocator->abi_version == LUNA_RUNTIME_ABI_V1 &&
        allocator->struct_size >= sizeof(LunaAllocatorV1) &&
        allocator->allocate && allocator->reallocate && allocator->deallocate;
}

bool validConsoleOutput(const LunaConsoleV1* console) {
    return console && console->abi_version == LUNA_RUNTIME_ABI_V1 &&
        console->struct_size >= LUNA_CONSOLE_V1_OUTPUT_SIZE &&
        console->write && console->flush;
}

bool validConsoleInput(const LunaConsoleV1* console) {
    return validConsoleOutput(console) &&
        console->struct_size >= sizeof(LunaConsoleV1) && console->read;
}

bool validExecutableMemory(const LunaExecutableMemoryV1* memory) {
    return memory && memory->abi_version == LUNA_RUNTIME_ABI_V1 &&
        memory->struct_size >= sizeof(LunaExecutableMemoryV1) &&
        memory->reserve && memory->seal && memory->release;
}

bool validFileSystem(const LunaFileSystemV1* filesystem) {
    return filesystem && filesystem->abi_version == LUNA_RUNTIME_ABI_V1 &&
        filesystem->struct_size >= sizeof(LunaFileSystemV1) &&
        filesystem->open && filesystem->read && filesystem->write &&
        filesystem->seek && filesystem->flush && filesystem->sync &&
        filesystem->close && filesystem->metadata &&
        filesystem->path_metadata &&
        filesystem->remove_file && filesystem->create_directory;
}

bool validHostServices(const LunaHostServicesV1* services) {
    if (!services || services->magic != LUNA_HOST_SERVICES_MAGIC_V1 ||
        services->abi_version != LUNA_RUNTIME_ABI_V1 ||
        services->struct_size < LUNA_HOST_SERVICES_V1_BASE_SIZE ||
        services->reserved_zero != 0 ||
        (services->capabilities & LUNA_HOST_CAP_ALLOCATOR) == 0 ||
        !validAllocator(services->allocator))
        return false;
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE) != 0 &&
        !validConsoleOutput(services->console))
        return false;
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE_INPUT) != 0 &&
        ((services->capabilities & LUNA_HOST_CAP_CONSOLE) == 0 ||
         !validConsoleInput(services->console)))
        return false;
    if ((services->capabilities & LUNA_HOST_CAP_EXECUTABLE_MEMORY) != 0 &&
        !validExecutableMemory(services->executable_memory))
        return false;
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) != 0 &&
        (services->struct_size < sizeof(LunaHostServicesV1) ||
         !validFileSystem(services->filesystem)))
        return false;
    return true;
}

int ioUnsupported(LunaIoErrorV1* error, uint32_t operation) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *error = {LUNA_RUNTIME_ABI_V1, sizeof(LunaIoErrorV1),
              LUNA_IO_ERROR_UNSUPPORTED, operation, 0};
    return LUNA_RUNTIME_STATUS_IO_ERROR;
}

int consoleAdapterFailure(LunaIoErrorV1* error, uint32_t operation,
                          int status) {
    if (status == LUNA_RUNTIME_STATUS_OK) return status;
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *error = {LUNA_RUNTIME_ABI_V1, sizeof(LunaIoErrorV1),
              status == LUNA_RUNTIME_STATUS_UNSUPPORTED_OPERATION
                  ? LUNA_IO_ERROR_UNSUPPORTED : LUNA_IO_ERROR_OTHER,
              operation, status};
    return LUNA_RUNTIME_STATUS_IO_ERROR;
}

int allocationFailure(LunaAllocErrorV1* error, uint32_t kind,
                      size_t requestedSize, size_t alignment) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *error = {LUNA_RUNTIME_ABI_V1, sizeof(LunaAllocErrorV1), kind, 0,
              static_cast<uint64_t>(requestedSize),
              static_cast<uint64_t>(alignment)};
    return LUNA_RUNTIME_STATUS_ALLOCATION_ERROR;
}

const LunaHostServicesV1* activateHostServices() {
    for (;;) {
        int phase = hostServicesPhase.load(std::memory_order_acquire);
        if (phase == 1)
            return installedHostServices.load(std::memory_order_acquire);
        if (phase == -1) continue;
        int expected = 0;
        if (hostServicesPhase.compare_exchange_weak(
                expected, 1, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return installedHostServices.load(std::memory_order_acquire);
    }
}

} // namespace

int rt_install_host_services_v1(const LunaHostServicesV1* services) {
    if (!services) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    if (services->abi_version != LUNA_RUNTIME_ABI_V1 ||
        services->magic != LUNA_HOST_SERVICES_MAGIC_V1)
        return LUNA_RUNTIME_STATUS_UNSUPPORTED_ABI;
    if (!validHostServices(services)) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    int expected = 0;
    if (!hostServicesPhase.compare_exchange_strong(
            expected, -1, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return LUNA_RUNTIME_STATUS_ALREADY_ACTIVE;
    installedHostServices.store(services, std::memory_order_release);
    hostServicesPhase.store(0, std::memory_order_release);
    return LUNA_RUNTIME_STATUS_OK;
}

const LunaAllocatorV1* lunaDefaultAllocatorV1() {
    return &defaultAllocator;
}

int lunaInstallApplicationHostServicesV1(const LunaHostServicesV1* services) {
    if (!services || !validHostServices(services))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    int expected = 0;
    if (!hostServicesPhase.compare_exchange_strong(
            expected, -1, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return LUNA_RUNTIME_STATUS_ALREADY_ACTIVE;
    if (installedHostServices.load(std::memory_order_acquire) !=
        &defaultHostServices) {
        hostServicesPhase.store(0, std::memory_order_release);
        return LUNA_RUNTIME_STATUS_ALREADY_ACTIVE;
    }
    installedHostServices.store(services,
                                std::memory_order_release);
    hostServicesPhase.store(0, std::memory_order_release);
    return LUNA_RUNTIME_STATUS_OK;
}

const LunaHostServicesV1* rt_host_services_v1() {
    return activateHostServices();
}

int rt_checked_array_layout_v1(size_t element_size, size_t element_count,
                               size_t alignment, size_t* byte_size,
                               LunaAllocErrorV1* error) {
    if (!byte_size || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *byte_size = 0;
    if (!isValidAlignment(alignment))
        return allocationFailure(error, LUNA_ALLOC_ERROR_INVALID_ALIGNMENT,
                                 0, alignment);
    if (element_size != 0 &&
        element_count > std::numeric_limits<size_t>::max() / element_size)
        return allocationFailure(error, LUNA_ALLOC_ERROR_SIZE_OVERFLOW,
                                 0, alignment);
    *byte_size = element_size * element_count;
    return LUNA_RUNTIME_STATUS_OK;
}

int rt_try_alloc_v1(size_t size, size_t alignment, void** allocation,
                    LunaAllocErrorV1* error) {
    if (!allocation || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *allocation = nullptr;
    if (!isValidAlignment(alignment))
        return allocationFailure(error, LUNA_ALLOC_ERROR_INVALID_ALIGNMENT,
                                 size, alignment);
    if (size == 0) return LUNA_RUNTIME_STATUS_OK;
    const auto* services = activateHostServices();
    void* result = services == &defaultHostServices
        ? defaultAllocate(nullptr, size, alignment)
        : services->allocator->allocate(
              services->allocator->context, size, alignment);
    if (!result)
        return allocationFailure(error, LUNA_ALLOC_ERROR_OUT_OF_MEMORY,
                                 size, alignment);
    *allocation = result;
    return LUNA_RUNTIME_STATUS_OK;
}

int rt_try_realloc_v1(void* pointer, size_t old_size, size_t new_size,
                      size_t alignment, void** replacement,
                      LunaAllocErrorV1* error) {
    if (!replacement || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *replacement = pointer;
    if (new_size == 0 || (!pointer && old_size != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    if (!isValidAlignment(alignment))
        return allocationFailure(error, LUNA_ALLOC_ERROR_INVALID_ALIGNMENT,
                                 new_size, alignment);
    if (!pointer)
        return rt_try_alloc_v1(new_size, alignment, replacement, error);
    const auto* services = activateHostServices();
    void* result = services == &defaultHostServices
        ? defaultReallocate(nullptr, pointer, old_size, new_size, alignment)
        : services->allocator->reallocate(
              services->allocator->context, pointer, old_size, new_size,
              alignment);
    if (!result)
        return allocationFailure(error, LUNA_ALLOC_ERROR_OUT_OF_MEMORY,
                                 new_size, alignment);
    *replacement = result;
    return LUNA_RUNTIME_STATUS_OK;
}

int rt_console_write_v1(uint32_t stream, const void* bytes, size_t byte_count,
                        LunaIoErrorV1* error) {
    if (!error || (!bytes && byte_count != 0) ||
        (stream != LUNA_CONSOLE_STDOUT && stream != LUNA_CONSOLE_STDERR))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_WRITE);
    return consoleAdapterFailure(
        error, LUNA_IO_OPERATION_WRITE,
        services->console->write(services->console->context, stream,
                                 static_cast<const char*>(bytes), byte_count));
}

int rt_console_flush_v1(uint32_t stream, LunaIoErrorV1* error) {
    if (!error ||
        (stream != LUNA_CONSOLE_STDOUT && stream != LUNA_CONSOLE_STDERR))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_FLUSH);
    return consoleAdapterFailure(
        error, LUNA_IO_OPERATION_FLUSH,
        services->console->flush(services->console->context, stream));
}

int rt_console_read_v1(void* bytes, size_t byte_capacity,
                       size_t* bytes_read, LunaIoErrorV1* error) {
    if (!bytes_read || !error || (!bytes && byte_capacity != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE_INPUT) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_READ);
    return services->console->read(services->console->context,
                                   static_cast<char*>(bytes), byte_capacity,
                                   bytes_read, error);
}

int rt_file_open_v1(const char* path_utf8, size_t path_size, uint32_t flags,
                    LunaFileHandleV1* handle, LunaIoErrorV1* error) {
    if (!handle || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_OPEN);
    return services->filesystem->open(services->filesystem->context,
                                      path_utf8, path_size, flags,
                                      handle, error);
}

int rt_file_read_v1(LunaFileHandleV1 handle, void* bytes,
                    size_t byte_capacity, size_t* bytes_read,
                    LunaIoErrorV1* error) {
    if (!bytes_read || !error || (!bytes && byte_capacity != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_READ);
    return services->filesystem->read(services->filesystem->context, handle,
                                      bytes, byte_capacity, bytes_read, error);
}

int rt_file_write_v1(LunaFileHandleV1 handle, const void* bytes,
                     size_t byte_count, size_t* bytes_written,
                     LunaIoErrorV1* error) {
    if (!bytes_written || !error || (!bytes && byte_count != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_WRITE);
    return services->filesystem->write(services->filesystem->context, handle,
                                       bytes, byte_count, bytes_written, error);
}

int rt_file_seek_v1(LunaFileHandleV1 handle, int64_t offset,
                    uint32_t whence, uint64_t* position,
                    LunaIoErrorV1* error) {
    if (!position || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_SEEK);
    return services->filesystem->seek(services->filesystem->context, handle,
                                      offset, whence, position, error);
}

int rt_file_flush_v1(LunaFileHandleV1 handle, LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_FLUSH);
    return services->filesystem->flush(services->filesystem->context,
                                       handle, error);
}

int rt_file_sync_v1(LunaFileHandleV1 handle, LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_SYNC);
    return services->filesystem->sync(services->filesystem->context,
                                      handle, error);
}

int rt_file_close_v1(LunaFileHandleV1 handle, LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_CLOSE);
    return services->filesystem->close(services->filesystem->context,
                                       handle, error);
}

int rt_file_metadata_v1(LunaFileHandleV1 handle,
                        LunaFileMetadataV1* metadata, LunaIoErrorV1* error) {
    if (!metadata || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_METADATA);
    return services->filesystem->metadata(services->filesystem->context,
                                          handle, metadata, error);
}

int rt_path_metadata_v1(const char* path_utf8, size_t path_size,
                        LunaFileMetadataV1* metadata, LunaIoErrorV1* error) {
    if (!metadata || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_METADATA);
    return services->filesystem->path_metadata(
        services->filesystem->context, path_utf8, path_size, metadata, error);
}

int rt_remove_file_v1(const char* path_utf8, size_t path_size,
                      LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_REMOVE_FILE);
    return services->filesystem->remove_file(
        services->filesystem->context, path_utf8, path_size, error);
}

int rt_create_directory_v1(const char* path_utf8, size_t path_size,
                           LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_FILESYSTEM) == 0)
        return ioUnsupported(error, LUNA_IO_OPERATION_CREATE_DIRECTORY);
    return services->filesystem->create_directory(
        services->filesystem->context, path_utf8, path_size, error);
}

void* rt_alloc(size_t size, size_t alignment) {
    if (!isValidAlignment(alignment)) return nullptr;
    const auto* services = activateHostServices();
    if (services == &defaultHostServices)
        return defaultAllocate(nullptr, size, alignment);
    return services->allocator->allocate(
        services->allocator->context, size, alignment);
}

void* rt_realloc(void* pointer, size_t old_size, size_t new_size,
                 size_t alignment) {
    if (!isValidAlignment(alignment)) return nullptr;
    const auto* services = activateHostServices();
    if (services == &defaultHostServices)
        return defaultReallocate(nullptr, pointer, old_size, new_size, alignment);
    const auto* allocator = services->allocator;
    if (!allocator->reallocate) return nullptr;
    return allocator->reallocate(
        allocator->context, pointer, old_size, new_size, alignment);
}

void rt_dealloc(void* pointer, size_t size, size_t alignment) {
    if (!pointer || !isValidAlignment(alignment)) return;
    const auto* services = activateHostServices();
    if (services == &defaultHostServices) {
        defaultDeallocate(nullptr, pointer, size, alignment);
        return;
    }
    services->allocator->deallocate(
        services->allocator->context, pointer, size, alignment);
}

static void* sharedAlloc(size_t size, size_t alignment,
                         LunaDropCallbackV1 drop, bool atomic) {
    if (!isValidAlignment(alignment)) return nullptr;
    const size_t effectiveAlignment =
        std::max(alignment, alignof(SharedAllocationHeader));
    const size_t total =
        sizeof(SharedAllocationHeader) + (size == 0 ? 1 : size) +
        effectiveAlignment - 1;
    void* allocation = rt_alloc(total, effectiveAlignment);
    if (!allocation) return nullptr;
    const uintptr_t first =
        reinterpret_cast<uintptr_t>(allocation) +
        sizeof(SharedAllocationHeader);
    const uintptr_t payload =
        (first + effectiveAlignment - 1) & ~(effectiveAlignment - 1);
    auto* header = reinterpret_cast<SharedAllocationHeader*>(
        payload - sizeof(SharedAllocationHeader));
    new (header) SharedAllocationHeader();
    header->allocation = allocation;
    header->allocationSize = total;
    header->allocationAlignment = effectiveAlignment;
    header->drop = drop;
    if (atomic)
        new (&header->count.arc) std::atomic<uint64_t>(1);
    else
        header->count.rc = 1;
    return reinterpret_cast<void*>(payload);
}

static void sharedDealloc(void* pointer, bool atomic) {
    if (!pointer) return;
    auto* header = sharedHeader(pointer);
    void* allocation = header->allocation;
    const size_t size = header->allocationSize;
    const size_t alignment = header->allocationAlignment;
    if (atomic) header->count.arc.~AtomicSharedCounter();
    header->~SharedAllocationHeader();
    rt_dealloc(allocation, size, alignment);
}

void* rt_rc_allocate_v1(int32_t size, int32_t alignment,
                        LunaDropCallbackV1 drop) {
    if (size < 0 || alignment <= 0) return nullptr;
    return sharedAlloc(static_cast<size_t>(size),
                       static_cast<size_t>(alignment), drop, false);
}

void rt_rc_retain_v1(void* pointer) {
    if (pointer) ++sharedHeader(pointer)->count.rc;
}

void rt_rc_release_v1(void* pointer) {
    if (!pointer) return;
    auto* header = sharedHeader(pointer);
    if (header->count.rc == 0 || --header->count.rc != 0) return;
    if (header->drop) header->drop(pointer);
    sharedDealloc(pointer, false);
}

void* rt_arc_allocate_v1(int32_t size, int32_t alignment,
                         LunaDropCallbackV1 drop) {
    if (size < 0 || alignment <= 0) return nullptr;
    return sharedAlloc(static_cast<size_t>(size),
                       static_cast<size_t>(alignment), drop, true);
}

void rt_arc_retain_v1(void* pointer) {
    if (pointer)
        sharedHeader(pointer)->count.arc.fetch_add(
            1, std::memory_order_relaxed);
}

void rt_arc_release_v1(void* pointer) {
    if (!pointer) return;
    auto* header = sharedHeader(pointer);
    if (header->count.arc.fetch_sub(
            1, std::memory_order_acq_rel) != 1) return;
    if (header->drop) header->drop(pointer);
    sharedDealloc(pointer, true);
}

void rt_panic_cstr(const char* message) {
    const char* text = message ? message : "panic";
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE) != 0 &&
        services->console) {
        const LunaConsoleV1* console = services->console;
        constexpr const char prefix[] = "Luna panic: ";
        constexpr const char newline[] = "\n";
        console->write(console->context, LUNA_CONSOLE_STDERR,
                       prefix, sizeof(prefix) - 1);
        console->write(console->context, LUNA_CONSOLE_STDERR,
                       text, std::strlen(text));
        console->write(console->context, LUNA_CONSOLE_STDERR,
                       newline, sizeof(newline) - 1);
        console->flush(console->context, LUNA_CONSOLE_STDERR);
    }
    std::abort();
}

void rt_print_i32(int32_t value) {
    char buffer[32];
    const int length = std::snprintf(buffer, sizeof(buffer), "%d\n", value);
    if (length <= 0) return;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE) == 0) return;
    const LunaConsoleV1* console = services->console;
    console->write(console->context, LUNA_CONSOLE_STDOUT, buffer,
                   static_cast<size_t>(length));
}

void rt_print_u32(uint32_t value) {
    char buffer[32];
    const int length = std::snprintf(buffer, sizeof(buffer), "%u\n", value);
    if (length <= 0) return;
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE) == 0) return;
    const LunaConsoleV1* console = services->console;
    console->write(console->context, LUNA_CONSOLE_STDOUT, buffer,
                   static_cast<size_t>(length));
}

void rt_print_cstr(const char* value) {
    const char* text = value ? value : "";
    const auto* services = activateHostServices();
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE) == 0) return;
    const LunaConsoleV1* console = services->console;
    console->write(console->context, LUNA_CONSOLE_STDOUT, text, std::strlen(text));
    console->write(console->context, LUNA_CONSOLE_STDOUT, "\n", 1);
}

int rt_console_write_cstr_v1(uint32_t stream, const char* value,
                            int32_t newline) {
    if (!value || (newline != 0 && newline != 1))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    LunaIoErrorV1 error{};
    int status = rt_console_write_v1(stream, value, std::strlen(value),
                                     &error);
    if (status == LUNA_RUNTIME_STATUS_OK && newline != 0)
        status = rt_console_write_v1(stream, "\n", 1, &error);
    return status;
}

int rt_console_write_i32_v1(uint32_t stream, int32_t value,
                           int32_t newline) {
    if (newline != 0 && newline != 1)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    char buffer[32];
    const int length = std::snprintf(buffer, sizeof(buffer), "%d", value);
    if (length <= 0) return LUNA_RUNTIME_STATUS_IO_ERROR;
    LunaIoErrorV1 error{};
    int status = rt_console_write_v1(
        stream, buffer, static_cast<size_t>(length), &error);
    if (status == LUNA_RUNTIME_STATUS_OK && newline != 0)
        status = rt_console_write_v1(stream, "\n", 1, &error);
    return status;
}

int rt_console_flush_simple_v1(uint32_t stream) {
    LunaIoErrorV1 error{};
    return rt_console_flush_v1(stream, &error);
}

const char* rt_console_read_line_lossy_v1() {
    thread_local std::array<char, 4096> line{};
    size_t length = 0;
    for (;;) {
        char byte = 0;
        size_t bytesRead = 0;
        LunaIoErrorV1 error{};
        if (rt_console_read_v1(&byte, 1, &bytesRead, &error) !=
                LUNA_RUNTIME_STATUS_OK ||
            bytesRead == 0)
            break;
        if (byte == '\n') break;
        if (length + 1 < line.size()) line[length++] = byte;
    }
    if (length != 0 && line[length - 1] == '\r') --length;
    line[length] = '\0';
    return line.data();
}

int32_t rt_parse_i32_or_v1(const char* text, int32_t fallback) {
    if (!text) return fallback;
    const char* begin = text;
    while (*begin == ' ' || *begin == '\t' || *begin == '\r' ||
           *begin == '\n')
        ++begin;
    const char* end = begin + std::strlen(begin);
    while (end != begin &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n'))
        --end;
    int32_t value = 0;
    const auto parsed = std::from_chars(begin, end, value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end && begin != end
        ? value : fallback;
}

int32_t rt_array_index_or_abort(int32_t index, size_t length) {
    if (index >= 0 && static_cast<size_t>(index) < length) return index;
    std::fprintf(stderr, "Luna runtime error: array index %d is outside length %zu\n", index, length);
    std::abort();
}
