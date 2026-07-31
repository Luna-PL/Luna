#include "runtime/Runtime.h"

#include <cstdlib>
#include <cstring>
#include <vector>

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
    return 0;
}
