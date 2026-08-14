#include <hip/hip_runtime.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

// C++23/HIP counterpart of the Luna heterogeneous scale suite. The legacy
// invocation (no arguments, or `--awaited`) keeps the original 16 Mi-element,
// 10-pass workload so existing scripts keep their 29524 checksum contract.
//
// New options mirror tools/gen_heterogeneous_scale.py parameters:
//   --elements=N   device element count (default 16*1024*1024)
//   --passes=P     transform passes (default 10)
//   --ops=K        v*3+1 repetitions per element per pass (default 1)
//   --mode=vector|transfer|launch   workload shape (default vector)
//   --launches=L   launch/await pairs for --mode=launch (default 1000)
//   --awaited      per-launch event create/record/sync/destroy lifecycle
//
// The checksum is printed as a signed decimal to match Luna's i32 print; a
// wrapped value therefore prints identically on both sides.

namespace {
constexpr int kDefaultElements = 16 * 1024 * 1024;
constexpr int kDefaultPasses = 10;
constexpr int kBlockSize = 256;
constexpr int kExpectedChecksum = 29524;

enum class Mode { kVector, kTransfer, kLaunch };

struct Options {
    int elements = kDefaultElements;
    int passes = kDefaultPasses;
    int ops = 1;
    int launches = 1000;
    Mode mode = Mode::kVector;
    bool awaited = false;
};

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

__global__ void transform(int* data, int count, int ops) {
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index < count) {
        int value = data[index];
        for (int o = 0; o < ops; ++o) value = value * 3 + 1;
        data[index] = value;
    }
}

__global__ void touch(int* data) {
    data[0] += 1;
}

struct Elapsed {
    float ms = 0.0F;
};

// Create, record, launch, record-stop, synchronize, destroy: the same
// lifecycle Luna's launch/await performs per dispatch.
void runAwaited(auto&& launch, Elapsed* out) {
    hipEvent_t start = nullptr;
    hipEvent_t stop = nullptr;
    check(hipEventCreate(&start), "hipEventCreate(start)");
    check(hipEventCreate(&stop), "hipEventCreate(stop)");
    check(hipEventRecord(start, nullptr), "hipEventRecord(start)");
    launch();
    check(hipGetLastError(), "launch");
    check(hipEventRecord(stop, nullptr), "hipEventRecord(stop)");
    check(hipEventSynchronize(stop), "hipEventSynchronize(stop)");
    float elapsedMs = 0.0F;
    check(hipEventElapsedTime(&elapsedMs, start, stop), "hipEventElapsedTime");
    check(hipEventDestroy(start), "hipEventDestroy(start)");
    check(hipEventDestroy(stop), "hipEventDestroy(stop)");
    out->ms += elapsedMs;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--awaited") == 0) {
            options.awaited = true;
        } else if (std::strcmp(arg, "--mode") == 0 && i + 1 < argc) {
            const char* mode = argv[++i];
            if (std::strcmp(mode, "vector") == 0) options.mode = Mode::kVector;
            else if (std::strcmp(mode, "transfer") == 0) options.mode = Mode::kTransfer;
            else if (std::strcmp(mode, "launch") == 0) options.mode = Mode::kLaunch;
            else {
                std::cerr << "unknown mode: " << mode << "\n";
                std::exit(2);
            }
        } else if (std::strncmp(arg, "--mode=", 7) == 0) {
            const char* mode = arg + 7;
            if (std::strcmp(mode, "vector") == 0) options.mode = Mode::kVector;
            else if (std::strcmp(mode, "transfer") == 0) options.mode = Mode::kTransfer;
            else if (std::strcmp(mode, "launch") == 0) options.mode = Mode::kLaunch;
            else {
                std::cerr << "unknown mode: " << mode << "\n";
                std::exit(2);
            }
        } else if (std::strcmp(arg, "--elements") == 0 && i + 1 < argc) {
            options.elements = std::atoi(argv[++i]);
        } else if (std::strncmp(arg, "--elements=", 11) == 0) {
            options.elements = std::atoi(arg + 11);
        } else if (std::strcmp(arg, "--passes") == 0 && i + 1 < argc) {
            options.passes = std::atoi(argv[++i]);
        } else if (std::strncmp(arg, "--passes=", 9) == 0) {
            options.passes = std::atoi(arg + 9);
        } else if (std::strcmp(arg, "--ops") == 0 && i + 1 < argc) {
            options.ops = std::atoi(argv[++i]);
        } else if (std::strncmp(arg, "--ops=", 6) == 0) {
            options.ops = std::atoi(arg + 6);
        } else if (std::strcmp(arg, "--launches") == 0 && i + 1 < argc) {
            options.launches = std::atoi(argv[++i]);
        } else if (std::strncmp(arg, "--launches=", 11) == 0) {
            options.launches = std::atoi(arg + 11);
        } else {
            std::cerr << "usage: cpp23_hip_vector [--awaited] [--mode vector|transfer|launch] "
                         "[--elements N] [--passes P] [--ops K] [--launches L]\n";
            std::exit(2);
        }
    }
    if (options.elements <= 0 || options.passes <= 0 || options.ops <= 0 ||
        options.launches <= 0) {
        std::cerr << "elements/passes/ops/launches must be positive\n";
        std::exit(2);
    }
    return options;
}

int runVector(const Options& options, int* data) {
    const dim3 block(kBlockSize);
    const dim3 grid((options.elements + kBlockSize - 1) / kBlockSize);
    Elapsed kernel;
    if (options.awaited) {
        runAwaited([&] {
            hipLaunchKernelGGL(initialize, grid, block, 0, nullptr, data, options.elements);
        }, &kernel);
        for (int pass = 0; pass < options.passes; ++pass) {
            runAwaited([&] {
                hipLaunchKernelGGL(transform, grid, block, 0, nullptr, data,
                                   options.elements, options.ops);
            }, &kernel);
        }
    } else {
        hipEvent_t start = nullptr;
        hipEvent_t stop = nullptr;
        check(hipEventCreate(&start), "hipEventCreate(start)");
        check(hipEventCreate(&stop), "hipEventCreate(stop)");
        check(hipEventRecord(start, nullptr), "hipEventRecord(start)");
        hipLaunchKernelGGL(initialize, grid, block, 0, nullptr, data, options.elements);
        check(hipGetLastError(), "initialize launch");
        for (int pass = 0; pass < options.passes; ++pass) {
            hipLaunchKernelGGL(transform, grid, block, 0, nullptr, data,
                               options.elements, options.ops);
            check(hipGetLastError(), "transform launch");
        }
        check(hipEventRecord(stop, nullptr), "hipEventRecord(stop)");
        check(hipEventSynchronize(stop), "hipEventSynchronize(stop)");
        check(hipEventElapsedTime(&kernel.ms, start, stop), "hipEventElapsedTime");
        check(hipEventDestroy(start), "hipEventDestroy(start)");
        check(hipEventDestroy(stop), "hipEventDestroy(stop)");
    }

    int checksum = 0;
    check(hipMemcpy(&checksum, data, sizeof(checksum), hipMemcpyDeviceToHost),
          "hipMemcpy(DeviceToHost)");
    std::cout << "mode=" << (options.awaited ? "awaited" : "stream") << "\n";
    std::cout << "kernel_ms=" << kernel.ms << "\n";
    std::cout << "checksum=" << checksum << "\n";
    return checksum;
}

void runTransfer(const Options& options) {
    const size_t bytes = static_cast<size_t>(options.elements) * sizeof(int);
    int* host = static_cast<int*>(std::calloc(options.elements, sizeof(int)));
    int* data = nullptr;
    check(hipMalloc(&data, bytes), "hipMalloc");
    check(hipMemset(data, 0, bytes), "hipMemset");

    hipEvent_t start = nullptr;
    hipEvent_t stop = nullptr;
    check(hipEventCreate(&start), "hipEventCreate(start)");
    check(hipEventCreate(&stop), "hipEventCreate(stop)");
    check(hipEventRecord(start, nullptr), "hipEventRecord(start)");
    check(hipMemcpy(data, host, bytes, hipMemcpyHostToDevice), "hipMemcpy(H2D)");
    check(hipMemcpy(host, data, bytes, hipMemcpyDeviceToHost), "hipMemcpy(D2H)");
    check(hipEventRecord(stop, nullptr), "hipEventRecord(stop)");
    check(hipEventSynchronize(stop), "hipEventSynchronize(stop)");
    float transferMs = 0.0F;
    check(hipEventElapsedTime(&transferMs, start, stop), "hipEventElapsedTime");
    check(hipEventDestroy(start), "hipEventDestroy(start)");
    check(hipEventDestroy(stop), "hipEventDestroy(stop)");

    int checksum = 0;
    check(hipMemcpy(&checksum, data, sizeof(checksum), hipMemcpyDeviceToHost),
          "hipMemcpy(DeviceToHost)");
    check(hipFree(data), "hipFree");
    std::free(host);
    std::cout << "mode=transfer\n";
    std::cout << "transfer_ms=" << transferMs << "\n";
    std::cout << "checksum=" << checksum << "\n";
}

void runLaunch(const Options& options) {
    int* data = nullptr;
    check(hipMalloc(&data, sizeof(int)), "hipMalloc");
    const int seed = 41;
    check(hipMemcpy(data, &seed, sizeof(seed), hipMemcpyHostToDevice),
          "hipMemcpy(H2D seed)");

    Elapsed kernel;
    for (int i = 0; i < options.launches; ++i) {
        runAwaited([&] {
            hipLaunchKernelGGL(touch, 1, 1, 0, nullptr, data);
        }, &kernel);
    }

    int checksum = 0;
    check(hipMemcpy(&checksum, data, sizeof(checksum), hipMemcpyDeviceToHost),
          "hipMemcpy(DeviceToHost)");
    check(hipFree(data), "hipFree");
    std::cout << "mode=launch\n";
    std::cout << "kernel_ms=" << kernel.ms << "\n";
    std::cout << "checksum=" << checksum << "\n";
}
} // namespace

int main(int argc, char** argv) {
    Options options = parseOptions(argc, argv);

    if (options.mode == Mode::kTransfer) {
        runTransfer(options);
        return 0;
    }
    if (options.mode == Mode::kLaunch) {
        runLaunch(options);
        return 0;
    }

    int* data = nullptr;
    check(hipMalloc(&data, static_cast<size_t>(options.elements) * sizeof(int)),
          "hipMalloc");
    const int checksum = runVector(options, data);
    check(hipFree(data), "hipFree");

    // Legacy contract: the default 64 MiB / 10-pass / ops=1 stream invocation
    // must still report 29524 and exit 0 so run_rocm_cpp23_comparison.sh holds.
    const bool legacy = options.elements == kDefaultElements &&
                        options.passes == kDefaultPasses && options.ops == 1 &&
                        !options.awaited;
    return legacy && checksum != kExpectedChecksum ? 2 : 0;
}
