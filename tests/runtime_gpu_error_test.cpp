#include "runtime/Runtime.h"

#include <cstdlib>
#include <cstring>

int main() {
    // The simulator is deterministic and available in ordinary CI.  It still
    // exercises the common event-state ABI used by CUDA and ROCm.
    setenv("LUNA_GPU_BACKEND", "sim", 1);
    if (!rt_gpu_initialize()) return 1;
    if (rt_gpu_await_event(1) != 1) return 2;
    if (rt_gpu_await_event(0) != 0) return 3;
    const char* error = rt_gpu_last_error();
    if (!error || !std::strstr(error, "launch did not create an event")) return 4;
    return 0;
}
