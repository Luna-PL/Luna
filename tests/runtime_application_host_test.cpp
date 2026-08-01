#include "runtime/Runtime.h"

#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "expected one existing UTF-8 file path\n";
        return 1;
    }
    if (rt_install_application_host_services_v1() !=
        LUNA_RUNTIME_STATUS_OK) {
        std::cerr << "application host services were not installed\n";
        return 1;
    }
    if (rt_install_application_host_services_v1() !=
        LUNA_RUNTIME_STATUS_ALREADY_ACTIVE) {
        std::cerr << "application host services replaced an installed table\n";
        return 1;
    }
    const LunaHostServicesV1* services = rt_host_services_v1();
    const uint64_t expectedCapabilities = LUNA_HOST_CAP_ALLOCATOR |
        LUNA_HOST_CAP_CONSOLE | LUNA_HOST_CAP_CONSOLE_INPUT |
        LUNA_HOST_CAP_FILESYSTEM;
    if ((services->capabilities & expectedCapabilities) !=
            expectedCapabilities ||
        !services->console || !services->console->read ||
        !services->filesystem) {
        std::cerr << "application host capability set is incomplete\n";
        return 1;
    }

    const std::string path = argv[1];
    LunaIoErrorV1 error{};
    LunaFileHandleV1 handle = LUNA_INVALID_FILE_HANDLE_V1;
    if (services->filesystem->open(
            services->filesystem->context, path.data(), path.size(),
            LUNA_FILE_OPEN_READ, &handle, &error) != LUNA_RUNTIME_STATUS_OK ||
        handle == LUNA_INVALID_FILE_HANDLE_V1) {
        std::cerr << "application filesystem could not open an existing file\n";
        return 1;
    }

    char prefix[8]{};
    size_t bytesRead = 0;
    if (services->filesystem->read(
            services->filesystem->context, handle, prefix, sizeof(prefix),
            &bytesRead, &error) != LUNA_RUNTIME_STATUS_OK ||
        bytesRead == 0 || std::memcmp(prefix, "fn value", 8) != 0) {
        std::cerr << "application filesystem read failed\n";
        return 1;
    }
    bytesRead = 1;
    if (services->filesystem->read(
            services->filesystem->context, handle, nullptr, 0,
            &bytesRead, &error) != LUNA_RUNTIME_STATUS_OK || bytesRead != 0) {
        std::cerr << "zero-length filesystem read failed\n";
        return 1;
    }

    LunaFileMetadataV1 byHandle{};
    LunaFileMetadataV1 byPath{};
    if (services->filesystem->metadata(
            services->filesystem->context, handle, &byHandle, &error) !=
            LUNA_RUNTIME_STATUS_OK ||
        services->filesystem->path_metadata(
            services->filesystem->context, path.data(), path.size(),
            &byPath, &error) != LUNA_RUNTIME_STATUS_OK ||
        byHandle.file_type != LUNA_FILE_TYPE_REGULAR ||
        byHandle.byte_size != byPath.byte_size || byHandle.byte_size == 0) {
        std::cerr << "application filesystem metadata failed\n";
        return 1;
    }
    uint64_t position = 0;
    if (services->filesystem->seek(
            services->filesystem->context, handle, -3, LUNA_SEEK_FROM_END,
            &position, &error) != LUNA_RUNTIME_STATUS_OK ||
        position != byHandle.byte_size - 3 ||
        services->filesystem->read(
            services->filesystem->context, handle, prefix, sizeof(prefix),
            &bytesRead, &error) != LUNA_RUNTIME_STATUS_OK || bytesRead != 3) {
        std::cerr << "application filesystem partial read/seek failed\n";
        return 1;
    }

    if (services->filesystem->close(
            services->filesystem->context, handle, &error) !=
            LUNA_RUNTIME_STATUS_OK ||
        services->filesystem->close(
            services->filesystem->context, handle, &error) !=
            LUNA_RUNTIME_STATUS_IO_ERROR ||
        error.operation != LUNA_IO_OPERATION_CLOSE) {
        std::cerr << "application filesystem close ownership failed\n";
        return 1;
    }

    const char invalidUtf8[] = {static_cast<char>(0xc0),
                                static_cast<char>(0x80)};
    handle = LUNA_INVALID_FILE_HANDLE_V1;
    if (services->filesystem->open(
            services->filesystem->context, invalidUtf8, sizeof(invalidUtf8),
            LUNA_FILE_OPEN_READ, &handle, &error) !=
            LUNA_RUNTIME_STATUS_IO_ERROR ||
        error.kind != LUNA_IO_ERROR_INVALID_INPUT ||
        error.operation != LUNA_IO_OPERATION_OPEN) {
        std::cerr << "invalid UTF-8 path was not rejected\n";
        return 1;
    }
    const char embeddedNul[] = {'a', '\0', 'b'};
    if (services->filesystem->path_metadata(
            services->filesystem->context, embeddedNul, sizeof(embeddedNul),
            &byPath, &error) != LUNA_RUNTIME_STATUS_IO_ERROR ||
        error.kind != LUNA_IO_ERROR_INVALID_INPUT) {
        std::cerr << "embedded-NUL path was not rejected\n";
        return 1;
    }
    if (services->filesystem->open(
            services->filesystem->context, path.data(), path.size(),
            LUNA_FILE_OPEN_WRITE | LUNA_FILE_OPEN_CREATE_NEW,
            &handle, &error) != LUNA_RUNTIME_STATUS_IO_ERROR ||
        error.kind != LUNA_IO_ERROR_ALREADY_EXISTS) {
        std::cerr << "create_new did not reject an existing path\n";
        return 1;
    }
    if (rt_console_flush_v1(LUNA_CONSOLE_STDOUT, &error) !=
        LUNA_RUNTIME_STATUS_OK) {
        std::cerr << "console forwarding bridge failed\n";
        return 1;
    }
    handle = LUNA_INVALID_FILE_HANDLE_V1;
    if (rt_file_open_v1(path.data(), path.size(), LUNA_FILE_OPEN_READ,
                        &handle, &error) != LUNA_RUNTIME_STATUS_OK ||
        rt_file_read_v1(handle, prefix, sizeof(prefix), &bytesRead, &error) !=
            LUNA_RUNTIME_STATUS_OK ||
        bytesRead != sizeof(prefix) ||
        rt_file_close_v1(handle, &error) != LUNA_RUNTIME_STATUS_OK) {
        std::cerr << "filesystem forwarding bridge failed\n";
        return 1;
    }
    return 0;
}
