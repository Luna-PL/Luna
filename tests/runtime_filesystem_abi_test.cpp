#include "runtime/Runtime.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

struct TestHost {
    size_t consoleReads = 0;
    size_t fileReads = 0;
};

void* allocate(void*, size_t size, size_t) {
    return std::malloc(size == 0 ? 1 : size);
}

void* reallocate(void*, void* pointer, size_t, size_t newSize, size_t) {
    return std::realloc(pointer, newSize);
}

void deallocate(void*, void* pointer, size_t, size_t) {
    std::free(pointer);
}

int writeConsole(void*, uint32_t, const char*, size_t) {
    return LUNA_RUNTIME_STATUS_OK;
}

int flushConsole(void*, uint32_t) {
    return LUNA_RUNTIME_STATUS_OK;
}

int readConsole(void* context, char* bytes, size_t capacity, size_t* bytesRead,
                LunaIoErrorV1*) {
    if (!bytesRead || (!bytes && capacity != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    auto& host = *static_cast<TestHost*>(context);
    if (host.consoleReads++ != 0) {
        *bytesRead = 0;
        return LUNA_RUNTIME_STATUS_OK;
    }
    if (capacity < 2)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    std::memcpy(bytes, "in", 2);
    *bytesRead = 2;
    return LUNA_RUNTIME_STATUS_OK;
}

int openFile(void*, const char* path, size_t pathSize, uint32_t flags,
             LunaFileHandleV1* handle, LunaIoErrorV1* error) {
    if (!path || !handle || !error || flags != LUNA_FILE_OPEN_READ)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    if (pathSize != 6 || std::memcmp(path, "sample", 6) != 0) {
        *error = {LUNA_RUNTIME_ABI_V1, sizeof(LunaIoErrorV1),
                  LUNA_IO_ERROR_NOT_FOUND, LUNA_IO_OPERATION_OPEN, 2};
        return LUNA_RUNTIME_STATUS_IO_ERROR;
    }
    *handle = 7;
    return LUNA_RUNTIME_STATUS_OK;
}

int readFile(void* context, LunaFileHandleV1 handle, void* bytes, size_t capacity,
             size_t* bytesRead, LunaIoErrorV1*) {
    if (handle != 7 || !bytesRead || (!bytes && capacity != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    auto& host = *static_cast<TestHost*>(context);
    if (host.fileReads++ != 0) {
        *bytesRead = 0;
        return LUNA_RUNTIME_STATUS_OK;
    }
    if (capacity < 2)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    std::memcpy(bytes, "ok", 2);
    *bytesRead = 2;
    return LUNA_RUNTIME_STATUS_OK;
}

int writeFile(void*, LunaFileHandleV1 handle, const void*, size_t count,
              size_t* bytesWritten, LunaIoErrorV1*) {
    if (handle != 7 || !bytesWritten)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *bytesWritten = count;
    return LUNA_RUNTIME_STATUS_OK;
}

int seekFile(void*, LunaFileHandleV1 handle, int64_t offset, uint32_t whence,
             uint64_t* position, LunaIoErrorV1*) {
    if (handle != 7 || offset < 0 || whence != LUNA_SEEK_FROM_START ||
        !position)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *position = static_cast<uint64_t>(offset);
    return LUNA_RUNTIME_STATUS_OK;
}

int fileOperation(void*, LunaFileHandleV1 handle, LunaIoErrorV1*) {
    return handle == 7 ? LUNA_RUNTIME_STATUS_OK
                       : LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
}

int metadataValue(LunaFileMetadataV1* value) {
    if (!value)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *value = {LUNA_RUNTIME_ABI_V1, sizeof(LunaFileMetadataV1),
              LUNA_FILE_TYPE_REGULAR, 0, 2};
    return LUNA_RUNTIME_STATUS_OK;
}

int handleMetadata(void*, LunaFileHandleV1 handle, LunaFileMetadataV1* value,
                   LunaIoErrorV1*) {
    return handle == 7 ? metadataValue(value)
                       : LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
}

int pathMetadata(void*, const char*, size_t, LunaFileMetadataV1* value,
                 LunaIoErrorV1*) {
    return metadataValue(value);
}

int pathOperation(void*, const char*, size_t, LunaIoErrorV1*) {
    return LUNA_RUNTIME_STATUS_OK;
}

int createDirectory(void*, const char*, size_t, LunaIoErrorV1*) {
    return LUNA_RUNTIME_STATUS_OK;
}

} // namespace

int main() {
    TestHost host;
    const LunaAllocatorV1 allocator{
        LUNA_RUNTIME_ABI_V1, sizeof(LunaAllocatorV1), nullptr,
        allocate, reallocate, deallocate};
    const LunaConsoleV1 console{
        LUNA_RUNTIME_ABI_V1, sizeof(LunaConsoleV1), &host,
        writeConsole, flushConsole, readConsole};
    const LunaFileSystemV1 filesystem{
        LUNA_RUNTIME_ABI_V1,
        sizeof(LunaFileSystemV1),
        &host,
        openFile,
        readFile,
        writeFile,
        seekFile,
        fileOperation,
        fileOperation,
        fileOperation,
        handleMetadata,
        pathMetadata,
        pathOperation,
        createDirectory,
    };
    const LunaHostServicesV1 services{
        LUNA_HOST_SERVICES_MAGIC_V1,
        LUNA_RUNTIME_ABI_V1,
        sizeof(LunaHostServicesV1),
        0,
        LUNA_HOST_CAP_ALLOCATOR | LUNA_HOST_CAP_CONSOLE |
            LUNA_HOST_CAP_CONSOLE_INPUT | LUNA_HOST_CAP_FILESYSTEM,
        &allocator,
        &console,
        nullptr,
        &filesystem,
    };
    if (rt_install_host_services_v1(&services) != LUNA_RUNTIME_STATUS_OK) {
        std::cerr << "valid filesystem service was rejected\n";
        return 1;
    }
    const auto* active = rt_host_services_v1();
    if (active != &services || active->filesystem != &filesystem) {
        std::cerr << "filesystem service was not retained\n";
        return 1;
    }
    char input[2]{};
    size_t inputSize = 0;
    LunaIoErrorV1 error{};
    if (active->console->read(active->console->context, input, sizeof(input),
                              &inputSize, &error) != LUNA_RUNTIME_STATUS_OK ||
        inputSize != 2 || std::memcmp(input, "in", 2) != 0) {
        std::cerr << "console input contract failed\n";
        return 1;
    }
    inputSize = 1;
    if (active->console->read(active->console->context, input, sizeof(input),
                              &inputSize, &error) != LUNA_RUNTIME_STATUS_OK ||
        inputSize != 0) {
        std::cerr << "console EOF contract failed\n";
        return 1;
    }
    LunaFileHandleV1 handle = LUNA_INVALID_FILE_HANDLE_V1;
    if (active->filesystem->open(
            active->filesystem->context, "absent", 6, LUNA_FILE_OPEN_READ,
            &handle, &error) != LUNA_RUNTIME_STATUS_IO_ERROR ||
        error.kind != LUNA_IO_ERROR_NOT_FOUND ||
        error.operation != LUNA_IO_OPERATION_OPEN || error.raw_code != 2) {
        std::cerr << "filesystem error identity contract failed\n";
        return 1;
    }
    if (active->filesystem->open(
            active->filesystem->context, "sample", 6, LUNA_FILE_OPEN_READ,
            &handle, &error) != LUNA_RUNTIME_STATUS_OK || handle != 7) {
        std::cerr << "filesystem open contract failed\n";
        return 1;
    }
    char bytes[2]{};
    size_t bytesRead = 0;
    if (active->filesystem->read(
            active->filesystem->context, handle, bytes, sizeof(bytes),
            &bytesRead, &error) != LUNA_RUNTIME_STATUS_OK ||
        bytesRead != 2 || std::memcmp(bytes, "ok", 2) != 0) {
        std::cerr << "filesystem read contract failed\n";
        return 1;
    }
    bytesRead = 1;
    if (active->filesystem->read(
            active->filesystem->context, handle, bytes, sizeof(bytes),
            &bytesRead, &error) != LUNA_RUNTIME_STATUS_OK || bytesRead != 0) {
        std::cerr << "filesystem EOF contract failed\n";
        return 1;
    }
    return 0;
}
