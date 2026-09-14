#include "runtime/Runtime.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

static_assert(sizeof(LunaDeviceBufferI32V1) ==
              sizeof(void*) + sizeof(size_t));
static_assert(offsetof(LunaDeviceBufferI32V1, data) == 0);
static_assert(offsetof(LunaDeviceBufferI32V1, length) == sizeof(void*));

int main() {
    // The simulator is deterministic and available in ordinary CI.  It still
    // exercises the common event-state ABI used by CUDA and ROCm.
    #ifdef _WIN32
    _putenv("LUNA_GPU_BACKEND=sim");
#else
    setenv("LUNA_GPU_BACKEND", "sim", 1);
#endif
    if (!rt_gpu_initialize()) return 1;
    LunaRuntimeErrorSnapshotV1 snapshot{};
    if (rt_runtime_error_snapshot_v1(
            LUNA_RUNTIME_ERROR_DOMAIN_GPU, &snapshot, nullptr, 0) !=
            LUNA_RUNTIME_STATUS_OK ||
        snapshot.code != LUNA_RUNTIME_ERROR_NONE ||
        snapshot.message_size != 0) {
        return 2;
    }
    if (rt_gpu_await_event(1) != 1) return 2;
    if (rt_gpu_await_event(0) != 0) return 3;
    // An operation error must not retroactively turn a successfully
    // initialized backend into an initialization failure.
    if (!rt_gpu_initialize()) return 4;
    const char* error = rt_gpu_last_error();
    if (!error || !std::strstr(error, "launch did not create an event")) return 5;

    if (rt_runtime_error_snapshot_v1(
            LUNA_RUNTIME_ERROR_DOMAIN_GPU, &snapshot, nullptr, 0) !=
            LUNA_RUNTIME_STATUS_BUFFER_TOO_SMALL) {
        return 6;
    }
    if (snapshot.abi_version != LUNA_RUNTIME_ABI_V1 ||
        snapshot.struct_size != sizeof(LunaRuntimeErrorSnapshotV1) ||
        snapshot.domain != LUNA_RUNTIME_ERROR_DOMAIN_GPU ||
        snapshot.code != LUNA_RUNTIME_ERROR_INVALID_STATE ||
        snapshot.message_size != std::strlen(error)) {
        return 7;
    }
    char truncated[8]{};
    if (rt_runtime_error_snapshot_v1(
            LUNA_RUNTIME_ERROR_DOMAIN_GPU, &snapshot, truncated,
            sizeof(truncated)) != LUNA_RUNTIME_STATUS_BUFFER_TOO_SMALL ||
        truncated[sizeof(truncated) - 1] != '\0' ||
        std::strncmp(truncated, error, sizeof(truncated) - 1) != 0) {
        return 8;
    }
    std::vector<char> ownedMessage(snapshot.message_size + 1);
    if (rt_runtime_error_snapshot_v1(
            LUNA_RUNTIME_ERROR_DOMAIN_GPU, &snapshot, ownedMessage.data(),
            ownedMessage.size()) != LUNA_RUNTIME_STATUS_OK ||
        std::strcmp(ownedMessage.data(), error) != 0) {
        return 9;
    }

    LunaDeviceBufferI32V1 buffer{};
    rt_gpu_alloc_i32(2, &buffer);
    if (!buffer.data || buffer.length != 2) return 10;
    rt_gpu_store_i32(buffer.data, buffer.length, 0, 17);
    rt_gpu_store_i32(buffer.data, buffer.length, 1, 23);
    if (rt_gpu_load_i32(buffer.data, buffer.length, 0) != 17 ||
        rt_gpu_load_i32(buffer.data, buffer.length, 1) != 23)
        return 11;

    LunaDeviceBufferI32V1 widened = buffer;
    widened.length = 3;
    if (rt_gpu_load_i32(widened.data, widened.length, 0) != 0 ||
        !std::strstr(rt_gpu_last_error(), "does not match its allocation length 2"))
        return 12;

    rt_gpu_store_i32(buffer.data, buffer.length, 2, 99);
    error = rt_gpu_last_error();
    if (!error || !std::strstr(error, "exceeds device buffer length 2"))
        return 13;
    if (rt_gpu_load_i32(buffer.data, buffer.length, -1) != 0 ||
        !std::strstr(rt_gpu_last_error(), "non-negative index"))
        return 14;

    int32_t host[3] = {1, 2, 3};
    if (rt_gpu_copy_from_host_i32(
            buffer.data, buffer.length, host, 3) != 0 ||
        !std::strstr(rt_gpu_last_error(), "exceeds device buffer length 2"))
        return 15;
    if (rt_gpu_copy_from_host_i32(
            buffer.data, buffer.length, host, 2) != 1)
        return 16;
    int32_t copied[2]{};
    if (rt_gpu_copy_to_host_i32(
            copied, buffer.data, buffer.length, 2) != 1 ||
        copied[0] != 1 || copied[1] != 2)
        return 17;

    rt_gpu_free(buffer.data, buffer.length);
    if (rt_gpu_load_i32(buffer.data, buffer.length, 0) != 0 ||
        !std::strstr(rt_gpu_last_error(), "requires a live buffer"))
        return 18;
    LunaDeviceBufferI32V1 overflow{};
    rt_gpu_alloc_i32(std::numeric_limits<size_t>::max(), &overflow);
    if (overflow.data != nullptr ||
        !std::strstr(rt_gpu_last_error(), "overflows"))
        return 19;
    return 0;
}
