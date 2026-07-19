#include <hip/hip_runtime.h>

#include <cstring>
#include <cstdlib>
#include <iostream>

namespace {
constexpr int kElements = 16 * 1024 * 1024;
constexpr int kBlockSize = 256;
constexpr int kTransformPasses = 10;
constexpr int kExpectedChecksum = 29524;

void check(hipError_t status, const char* operation) {
    if (status != hipSuccess) {
        std::cerr << operation << ": " << hipGetErrorString(status) << "\n";
        std::exit(1);
    }
}

__global__ void initialize(int* data, int count) {
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index < count) data[index] = index;
}

__global__ void transform(int* data, int count) {
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index < count) data[index] = data[index] * 3 + 1;
}
} // namespace

int main(int argc, char** argv) {
    const bool awaitEachLaunch = argc == 2 && std::strcmp(argv[1], "--awaited") == 0;
    if (argc > 2 || (argc == 2 && !awaitEachLaunch)) {
        std::cerr << "usage: cpp23_hip_vector [--awaited]\n";
        return 2;
    }
    int* data = nullptr;
    check(hipMalloc(&data, static_cast<size_t>(kElements) * sizeof(int)), "hipMalloc");

    const dim3 block(kBlockSize);
    const dim3 grid((kElements + kBlockSize - 1) / kBlockSize);
    float kernelMs = 0.0F;
    if (awaitEachLaunch) {
        // Match Luna's explicit launch/await lifecycle: each dispatch gets
        // start/completion events, then the host synchronizes before the next
        // borrowed use of the device buffer.
        auto runAwaited = [&](auto&& launch, const char* name) {
            hipEvent_t start = nullptr;
            hipEvent_t stop = nullptr;
            check(hipEventCreate(&start), "hipEventCreate(start)");
            check(hipEventCreate(&stop), "hipEventCreate(stop)");
            check(hipEventRecord(start, nullptr), "hipEventRecord(start)");
            launch();
            check(hipGetLastError(), name);
            check(hipEventRecord(stop, nullptr), "hipEventRecord(stop)");
            check(hipEventSynchronize(stop), "hipEventSynchronize(stop)");
            float elapsedMs = 0.0F;
            check(hipEventElapsedTime(&elapsedMs, start, stop), "hipEventElapsedTime");
            check(hipEventDestroy(start), "hipEventDestroy(start)");
            check(hipEventDestroy(stop), "hipEventDestroy(stop)");
            kernelMs += elapsedMs;
        };
        runAwaited([&] {
            hipLaunchKernelGGL(initialize, grid, block, 0, nullptr, data, kElements);
        }, "initialize launch");
        for (int pass = 0; pass < kTransformPasses; ++pass) {
            runAwaited([&] {
                hipLaunchKernelGGL(transform, grid, block, 0, nullptr, data, kElements);
            }, "transform launch");
        }
    } else {
        hipEvent_t start = nullptr;
        hipEvent_t stop = nullptr;
        check(hipEventCreate(&start), "hipEventCreate(start)");
        check(hipEventCreate(&stop), "hipEventCreate(stop)");
        check(hipEventRecord(start, nullptr), "hipEventRecord(start)");
        hipLaunchKernelGGL(initialize, grid, block, 0, nullptr, data, kElements);
        check(hipGetLastError(), "initialize launch");
        for (int pass = 0; pass < kTransformPasses; ++pass) {
            hipLaunchKernelGGL(transform, grid, block, 0, nullptr, data, kElements);
            check(hipGetLastError(), "transform launch");
        }
        check(hipEventRecord(stop, nullptr), "hipEventRecord(stop)");
        check(hipEventSynchronize(stop), "hipEventSynchronize(stop)");
        check(hipEventElapsedTime(&kernelMs, start, stop), "hipEventElapsedTime");
        check(hipEventDestroy(start), "hipEventDestroy(start)");
        check(hipEventDestroy(stop), "hipEventDestroy(stop)");
    }

    int checksum = 0;
    check(hipMemcpy(&checksum, data, sizeof(checksum), hipMemcpyDeviceToHost),
          "hipMemcpy(DeviceToHost)");
    check(hipFree(data), "hipFree");

    std::cout << "mode=" << (awaitEachLaunch ? "awaited" : "stream") << "\n";
    std::cout << "kernel_ms=" << kernelMs << "\n";
    std::cout << "checksum=" << checksum << "\n";
    return checksum == kExpectedChecksum ? 0 : 2;
}
