#include "Runtime.h"
#include "ApplicationHostServices.h"
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

// Keep native dynamic loading behind one adapter. CUDA/HIP headers are not a
// build dependency, and the simulator remains usable without vendor SDKs.
void* lunaOpenLibrary(const char* path) {
#ifdef _WIN32
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* lunaLoadSymbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(
        reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void lunaCloseLibrary(void* library) {
    if (!library) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

std::string lunaDynamicLoaderError() {
#ifdef _WIN32
    return "Windows loader error " + std::to_string(GetLastError());
#else
    const char* error = dlerror();
    return error ? std::string(error) : "unknown dynamic-loader error";
#endif
}

// Keep the CUDA Driver API deliberately private to this translation unit. The
// CUDA Toolkit headers are not a build dependency, and the symbols are loaded
// only when the user explicitly selects LUNA_GPU_BACKEND=cuda.
using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
using CUcontext = void*;
using CUmodule = void*;
using CUfunction = void*;
using CUstream = void*;
using CUevent = void*;
constexpr CUresult CUDA_SUCCESS = 0;

using CuInit = CUresult (*)(unsigned int);
using CuDeviceGet = CUresult (*)(CUdevice*, int);
using CuCtxCreate = CUresult (*)(CUcontext*, unsigned int, CUdevice);
using CuMemAlloc = CUresult (*)(CUdeviceptr*, size_t);
using CuMemFree = CUresult (*)(CUdeviceptr);
using CuMemcpyHtoD = CUresult (*)(CUdeviceptr, const void*, size_t);
using CuMemcpyDtoH = CUresult (*)(void*, CUdeviceptr, size_t);
using CuModuleLoadData = CUresult (*)(CUmodule*, const void*);
using CuModuleGetFunction = CUresult (*)(CUfunction*, CUmodule, const char*);
using CuLaunchKernel = CUresult (*)(CUfunction, unsigned int, unsigned int,
                                    unsigned int, unsigned int, unsigned int,
                                    unsigned int, unsigned int, CUstream,
                                    void**, void**);
using CuEventCreate = CUresult (*)(CUevent*, unsigned int);
using CuEventRecord = CUresult (*)(CUevent, CUstream);
using CuEventSynchronize = CUresult (*)(CUevent);
using CuEventDestroy = CUresult (*)(CUevent);
using CuEventElapsedTime = CUresult (*)(float*, CUevent, CUevent);

using hipError_t = int;
using hipModule_t = void*;
using hipFunction_t = void*;
using hipStream_t = void*;
using hipEvent_t = void*;
constexpr hipError_t hipSuccess = 0;
constexpr int hipMemcpyHostToDevice = 1;
constexpr int hipMemcpyDeviceToHost = 2;

using HipInit = hipError_t (*)(unsigned int);
using HipSetDevice = hipError_t (*)(int);
using HipMalloc = hipError_t (*)(void**, size_t);
using HipFree = hipError_t (*)(void*);
using HipMemcpy = hipError_t (*)(void*, const void*, size_t, int);
using HipModuleLoadData = hipError_t (*)(hipModule_t*, const void*);
using HipModuleGetFunction = hipError_t (*)(hipFunction_t*, hipModule_t, const char*);
using HipModuleLaunchKernel = hipError_t (*)(hipFunction_t, unsigned int, unsigned int,
                                              unsigned int, unsigned int, unsigned int,
                                              unsigned int, unsigned int, hipStream_t,
                                              void**, void**);
using HipEventCreate = hipError_t (*)(hipEvent_t*);
using HipEventRecord = hipError_t (*)(hipEvent_t, hipStream_t);
using HipEventSynchronize = hipError_t (*)(hipEvent_t);
using HipEventDestroy = hipError_t (*)(hipEvent_t);
using HipEventElapsedTime = hipError_t (*)(float*, hipEvent_t, hipEvent_t);
using HipGetErrorString = const char* (*)(hipError_t);

struct CudaApi {
    void* library = nullptr;
    CuInit init = nullptr;
    CuDeviceGet deviceGet = nullptr;
    CuCtxCreate ctxCreate = nullptr;
    CuMemAlloc memAlloc = nullptr;
    CuMemFree memFree = nullptr;
    CuMemcpyHtoD memcpyHtoD = nullptr;
    CuMemcpyDtoH memcpyDtoH = nullptr;
    CuModuleLoadData moduleLoadData = nullptr;
    CuModuleGetFunction moduleGetFunction = nullptr;
    CuLaunchKernel launchKernel = nullptr;
    CuEventCreate eventCreate = nullptr;
    CuEventRecord eventRecord = nullptr;
    CuEventSynchronize eventSynchronize = nullptr;
    CuEventDestroy eventDestroy = nullptr;
    CuEventElapsedTime eventElapsedTime = nullptr;
};

struct HipApi {
    void* library = nullptr;
    HipInit init = nullptr;
    HipSetDevice setDevice = nullptr;
    HipMalloc malloc = nullptr;
    HipFree free = nullptr;
    HipMemcpy memcpy = nullptr;
    HipModuleLoadData moduleLoadData = nullptr;
    HipModuleGetFunction moduleGetFunction = nullptr;
    HipModuleLaunchKernel launchKernel = nullptr;
    HipEventCreate eventCreate = nullptr;
    HipEventRecord eventRecord = nullptr;
    HipEventSynchronize eventSynchronize = nullptr;
    HipEventDestroy eventDestroy = nullptr;
    HipEventElapsedTime eventElapsedTime = nullptr;
    HipGetErrorString getErrorString = nullptr;
};

struct CudaEventRecord {
    CUevent start = nullptr;
    CUevent completion = nullptr;
};

struct HipPendingEvent {
    hipEvent_t start = nullptr;
    hipEvent_t completion = nullptr;
};

struct GpuRuntimeState {
    bool initialized = false;
    bool initializationSucceeded = false;
    bool cuda = false;
    bool rocm = false;
    std::string backend = "sim";
    int32_t errorCode = LUNA_RUNTIME_ERROR_NONE;
    std::string error;
    CudaApi api;
    HipApi hip;
    CUcontext context = nullptr;
    std::unordered_map<std::string, CUmodule> modules;
    std::unordered_map<std::string, CUfunction> functions;
    std::unordered_map<int32_t, CudaEventRecord> events;
    std::unordered_map<std::string, hipModule_t> hipModules;
    std::unordered_map<std::string, hipFunction_t> hipFunctions;
    std::unordered_map<int32_t, HipPendingEvent> hipEvents;
    std::mutex allocationMutex;
    std::unordered_map<void*, size_t> allocations;
    int32_t nextEvent = 1;
    bool profileEnabled = false;
    bool profileReporterRegistered = false;
    double profiledKernelMs = 0.0;
};

GpuRuntimeState& state() {
    static GpuRuntimeState value;
    return value;
}

void setGpuError(int32_t code, std::string message) {
    auto& runtime = state();
    runtime.errorCode = code;
    runtime.error = std::move(message);
}

bool gpuProfilingRequested() {
    const char* requested = std::getenv("LUNA_GPU_PROFILE");
    return requested && std::strcmp(requested, "1") == 0;
}

void reportGpuProfileAtExit() {
    const auto& runtime = state();
    if (!runtime.profileEnabled || (!runtime.cuda && !runtime.rocm)) return;
    std::printf("Luna GPU profile: kernel_ms=%.6f\n", runtime.profiledKernelMs);
}

// A module can export more than one kernel. Keep the binary in the key so a
// newly emitted code object cannot accidentally reuse a function handle from
// an older module with the same kernel name.
std::string kernelFunctionCacheKey(const std::string& moduleKey,
                                   const char* kernelName) {
    std::string key = moduleKey;
    key.push_back('\0');
    key.append(kernelName ? kernelName : "");
    return key;
}

template <typename Api, typename T>
bool loadRuntimeSymbol(Api& api, T& target, const char* name) {
    target = reinterpret_cast<T>(lunaLoadSymbol(api.library, name));
    return target != nullptr;
}

void setCudaError(const char* operation, CUresult status) {
    setGpuError(
        LUNA_RUNTIME_ERROR_BACKEND_OPERATION,
        std::string("CUDA Driver API call '") + operation +
            "' failed with code " + std::to_string(status));
}

bool checkCuda(const char* operation, CUresult status) {
    if (status == CUDA_SUCCESS) return true;
    setCudaError(operation, status);
    return false;
}

bool loadCudaApi() {
    auto& runtime = state();
#ifdef _WIN32
    constexpr const char* libraryName = "nvcuda.dll";
#else
    constexpr const char* libraryName = "libcuda.so.1";
#endif
    runtime.api.library = lunaOpenLibrary(libraryName);
    if (!runtime.api.library) {
        setGpuError(
            LUNA_RUNTIME_ERROR_BACKEND_UNAVAILABLE,
            "could not load " + std::string(libraryName) + ": " +
                lunaDynamicLoaderError());
        return false;
    }
    CudaApi& api = runtime.api;
    const bool symbolsLoaded =
        loadRuntimeSymbol(api, api.init, "cuInit") &&
        loadRuntimeSymbol(api, api.deviceGet, "cuDeviceGet") &&
        (loadRuntimeSymbol(api, api.ctxCreate, "cuCtxCreate_v2") ||
         loadRuntimeSymbol(api, api.ctxCreate, "cuCtxCreate")) &&
        (loadRuntimeSymbol(api, api.memAlloc, "cuMemAlloc_v2") ||
         loadRuntimeSymbol(api, api.memAlloc, "cuMemAlloc")) &&
        (loadRuntimeSymbol(api, api.memFree, "cuMemFree_v2") ||
         loadRuntimeSymbol(api, api.memFree, "cuMemFree")) &&
        (loadRuntimeSymbol(api, api.memcpyHtoD, "cuMemcpyHtoD_v2") ||
         loadRuntimeSymbol(api, api.memcpyHtoD, "cuMemcpyHtoD")) &&
        (loadRuntimeSymbol(api, api.memcpyDtoH, "cuMemcpyDtoH_v2") ||
         loadRuntimeSymbol(api, api.memcpyDtoH, "cuMemcpyDtoH")) &&
        loadRuntimeSymbol(api, api.moduleLoadData, "cuModuleLoadData") &&
        loadRuntimeSymbol(api, api.moduleGetFunction, "cuModuleGetFunction") &&
        loadRuntimeSymbol(api, api.launchKernel, "cuLaunchKernel") &&
        loadRuntimeSymbol(api, api.eventCreate, "cuEventCreate") &&
        loadRuntimeSymbol(api, api.eventRecord, "cuEventRecord") &&
        loadRuntimeSymbol(api, api.eventSynchronize, "cuEventSynchronize") &&
        (loadRuntimeSymbol(api, api.eventDestroy, "cuEventDestroy_v2") ||
         loadRuntimeSymbol(api, api.eventDestroy, "cuEventDestroy")) &&
        loadRuntimeSymbol(api, api.eventElapsedTime, "cuEventElapsedTime");
    if (!symbolsLoaded) {
        setGpuError(
            LUNA_RUNTIME_ERROR_MISSING_SYMBOL,
            std::string(libraryName) +
                " is missing a required CUDA Driver API symbol");
        lunaCloseLibrary(api.library);
        api.library = nullptr;
        return false;
    }
    return true;
}

void setHipError(const char* operation, hipError_t status) {
    std::string message = std::string("HIP runtime call '") + operation +
        "' failed with code " + std::to_string(status);
    if (state().hip.getErrorString) {
        const char* description = state().hip.getErrorString(status);
        if (description && *description)
            message += " (" + std::string(description) + ")";
    }
    setGpuError(LUNA_RUNTIME_ERROR_BACKEND_OPERATION, std::move(message));
}

bool checkHip(const char* operation, hipError_t status) {
    if (status == hipSuccess) return true;
    setHipError(operation, status);
    return false;
}

void setGpuOperationError(const std::string& message,
                          int32_t code = LUNA_RUNTIME_ERROR_INVALID_STATE) {
    setGpuError(code, message);
}

bool rememberGpuAllocation(void* buffer, size_t elementCount) {
    if (!buffer) return false;
    try {
        auto& runtime = state();
        std::lock_guard<std::mutex> lock(runtime.allocationMutex);
        return runtime.allocations.emplace(buffer, elementCount).second;
    } catch (const std::bad_alloc&) {
        setGpuOperationError(
            "could not record GPU allocation bounds",
            LUNA_RUNTIME_ERROR_BACKEND_OPERATION);
        return false;
    }
}

bool forgetGpuAllocation(void* buffer) {
    auto& runtime = state();
    std::lock_guard<std::mutex> lock(runtime.allocationMutex);
    return runtime.allocations.erase(buffer) == 1;
}

bool validateGpuRange(LunaDeviceBufferI32V1 buffer, size_t elementOffset,
                      size_t elementCount, const char* operation) {
    auto& runtime = state();
    std::lock_guard<std::mutex> lock(runtime.allocationMutex);
    const auto found = runtime.allocations.find(buffer.data);
    if (found == runtime.allocations.end()) {
        setGpuOperationError(
            std::string(operation) +
                " requires a live buffer returned by gpu_alloc_i32",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return false;
    }
    if (buffer.length != found->second) {
        setGpuOperationError(
            std::string(operation) + " device buffer length " +
                std::to_string(buffer.length) +
                " does not match its allocation length " +
                std::to_string(found->second),
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return false;
    }
    if (elementOffset > buffer.length ||
        elementCount > buffer.length - elementOffset) {
        setGpuOperationError(
            std::string(operation) + " range [" +
                std::to_string(elementOffset) + ", " +
                std::to_string(elementOffset + elementCount) +
                ") exceeds device buffer length " +
                std::to_string(buffer.length),
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return false;
    }
    return true;
}

bool loadHipApi() {
    auto& runtime = state();
#ifdef _WIN32
    constexpr const char* candidates[] = {"amdhip64.dll"};
#else
    constexpr const char* candidates[] = {"libamdhip64.so", "libamdhip64.so.6", "libamdhip64.so.5"};
#endif
    for (const char* candidate : candidates) {
        runtime.hip.library = lunaOpenLibrary(candidate);
        if (runtime.hip.library) break;
    }
    if (!runtime.hip.library) {
        setGpuError(
            LUNA_RUNTIME_ERROR_BACKEND_UNAVAILABLE,
            "could not load HIP runtime library (install the ROCm HIP runtime): " +
                lunaDynamicLoaderError());
        return false;
    }
    HipApi& api = runtime.hip;
    const bool symbolsLoaded =
        loadRuntimeSymbol(api, api.init, "hipInit") &&
        loadRuntimeSymbol(api, api.setDevice, "hipSetDevice") &&
        loadRuntimeSymbol(api, api.malloc, "hipMalloc") &&
        loadRuntimeSymbol(api, api.free, "hipFree") &&
        loadRuntimeSymbol(api, api.memcpy, "hipMemcpy") &&
        loadRuntimeSymbol(api, api.moduleLoadData, "hipModuleLoadData") &&
        loadRuntimeSymbol(api, api.moduleGetFunction, "hipModuleGetFunction") &&
        loadRuntimeSymbol(api, api.launchKernel, "hipModuleLaunchKernel") &&
        loadRuntimeSymbol(api, api.eventCreate, "hipEventCreate") &&
        loadRuntimeSymbol(api, api.eventRecord, "hipEventRecord") &&
        loadRuntimeSymbol(api, api.eventSynchronize, "hipEventSynchronize") &&
        loadRuntimeSymbol(api, api.eventDestroy, "hipEventDestroy") &&
        loadRuntimeSymbol(api, api.eventElapsedTime, "hipEventElapsedTime") &&
        loadRuntimeSymbol(api, api.getErrorString, "hipGetErrorString");
    if (!symbolsLoaded) {
        setGpuError(
            LUNA_RUNTIME_ERROR_MISSING_SYMBOL,
            "HIP runtime library is missing a required HIP Module API symbol");
        lunaCloseLibrary(api.library);
        api.library = nullptr;
        return false;
    }
    return true;
}

CUdeviceptr asDevicePointer(void* buffer) {
    return static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(buffer));
}

void* asOpaquePointer(CUdeviceptr buffer) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(buffer));
}

} // namespace

int rt_runtime_error_snapshot_v1(uint32_t domain,
                                 LunaRuntimeErrorSnapshotV1* snapshot,
                                 char* message, size_t message_capacity) {
    if (!snapshot || (message_capacity != 0 && !message))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;

    int32_t code = LUNA_RUNTIME_ERROR_NONE;
    const std::string* diagnostic = nullptr;
    switch (domain) {
    case LUNA_RUNTIME_ERROR_DOMAIN_GPU: {
        const auto& runtime = state();
        code = runtime.errorCode;
        diagnostic = &runtime.error;
        break;
    }
    default:
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    }

    snapshot->abi_version = LUNA_RUNTIME_ABI_V1;
    snapshot->struct_size = sizeof(LunaRuntimeErrorSnapshotV1);
    snapshot->domain = domain;
    snapshot->code = code;
    snapshot->message_size = diagnostic->size();

    if (message_capacity != 0) {
        const size_t copied = std::min(
            diagnostic->size(), message_capacity - 1);
        if (copied != 0)
            std::memmove(message, diagnostic->data(), copied);
        message[copied] = '\0';
    }
    return message_capacity > diagnostic->size()
        ? LUNA_RUNTIME_STATUS_OK
        : (diagnostic->empty() ? LUNA_RUNTIME_STATUS_OK
                               : LUNA_RUNTIME_STATUS_BUFFER_TOO_SMALL);
}

int rt_gpu_initialize() {
    auto& runtime = state();
    if (runtime.initialized) return runtime.initializationSucceeded ? 1 : 0;
    runtime.initialized = true;
    runtime.profileEnabled = gpuProfilingRequested();
    if (runtime.profileEnabled && !runtime.profileReporterRegistered) {
        std::atexit(reportGpuProfileAtExit);
        runtime.profileReporterRegistered = true;
    }
    const char* requested = std::getenv("LUNA_GPU_BACKEND");
    if (!requested || std::strcmp(requested, "sim") == 0 ||
        std::strcmp(requested, "cpu") == 0) {
        runtime.initializationSucceeded = true;
        return 1;
    }
    runtime.backend = requested;
    if (std::strcmp(requested, "cuda") == 0) {
        runtime.backend = "cuda";
        if (!loadCudaApi()) return 0;
        if (!checkCuda("cuInit", runtime.api.init(0))) return 0;
        CUdevice device = 0;
        if (!checkCuda("cuDeviceGet", runtime.api.deviceGet(&device, 0))) return 0;
        if (!checkCuda("cuCtxCreate", runtime.api.ctxCreate(&runtime.context, 0, device))) return 0;
        runtime.cuda = true;
        runtime.initializationSucceeded = true;
        return 1;
    }
    if (std::strcmp(requested, "rocm") == 0 || std::strcmp(requested, "hip") == 0) {
        runtime.backend = "rocm";
        if (!loadHipApi()) return 0;
        if (!checkHip("hipInit", runtime.hip.init(0))) return 0;
        if (!checkHip("hipSetDevice", runtime.hip.setDevice(0))) return 0;
        runtime.rocm = true;
        runtime.initializationSucceeded = true;
        return 1;
    }
    {
        setGpuError(
            LUNA_RUNTIME_ERROR_BACKEND_UNAVAILABLE,
            std::string("unknown GPU backend '") + requested +
                "'; use 'sim', 'cuda', or 'rocm'");
        return 0;
    }
}

const char* rt_gpu_backend_name() {
    return state().backend.c_str();
}

const char* rt_gpu_last_error() {
    return state().error.c_str();
}

void rt_gpu_report_initialization_error() {
    auto& runtime = state();
    std::fprintf(stderr, "GPU backend initialization failed for '%s': %s\n",
                 runtime.backend.c_str(), runtime.error.c_str());
}

void rt_gpu_report_operation_error_and_abort() {
    auto& runtime = state();
    const char* message = runtime.error.empty()
        ? "unknown GPU launch or synchronization failure"
        : runtime.error.c_str();
    std::fprintf(stderr, "GPU backend operation failed for '%s': %s\n",
                 runtime.backend.c_str(), message);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

int rt_gpu_backend_is_cuda() {
    return rt_gpu_initialize() && state().cuda ? 1 : 0;
}

int rt_gpu_backend_is_rocm() {
    return rt_gpu_initialize() && state().rocm ? 1 : 0;
}

void rt_gpu_alloc_i32(size_t element_count, LunaDeviceBufferI32V1* buffer) {
    if (!buffer) {
        setGpuOperationError(
            "gpu_alloc_i32 requires a non-null output carrier",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return;
    }
    *buffer = {nullptr, 0};
    if (!rt_gpu_initialize()) return;
    if (element_count > std::numeric_limits<size_t>::max() / sizeof(int32_t)) {
        setGpuOperationError(
            "gpu_alloc_i32 element count overflows the platform allocation size",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return;
    }
    const size_t bytes = element_count * sizeof(int32_t);
    void* allocation = nullptr;
    if (state().cuda) {
        CUdeviceptr buffer = 0;
        if (!checkCuda("cuMemAlloc", state().api.memAlloc(&buffer, bytes)))
            return;
        allocation = asOpaquePointer(buffer);
    } else if (state().rocm) {
        if (!checkHip("hipMalloc", state().hip.malloc(&allocation, bytes)))
            return;
    } else {
        allocation = std::calloc(element_count, sizeof(int32_t));
    }
    if (!allocation) return;
    if (rememberGpuAllocation(allocation, element_count)) {
        *buffer = {allocation, element_count};
        return;
    }
    if (state().cuda)
        state().api.memFree(asDevicePointer(allocation));
    else if (state().rocm)
        state().hip.free(allocation);
    else
        std::free(allocation);
}

void rt_gpu_free(void* data, size_t length) {
    const LunaDeviceBufferI32V1 buffer{data, length};
    if (!rt_gpu_initialize() || !data) return;
    if (!validateGpuRange(buffer, 0, 0, "gpu_free")) return;
    // Remove the capability before releasing its storage. Even if a vendor
    // free reports an error, no later operation may safely reuse that handle.
    (void)forgetGpuAllocation(buffer.data);
    if (state().cuda) {
        (void)checkCuda(
            "cuMemFree", state().api.memFree(asDevicePointer(buffer.data)));
    } else if (state().rocm) {
        (void)checkHip("hipFree", state().hip.free(buffer.data));
    } else {
        std::free(buffer.data);
    }
}

int32_t rt_gpu_load_i32(void* data, size_t length, int32_t index) {
    const LunaDeviceBufferI32V1 buffer{data, length};
    if (!rt_gpu_initialize() || !data) return 0;
    if (index < 0 ||
        !validateGpuRange(buffer, static_cast<size_t>(index), 1,
                          "gpu_load_i32")) {
        if (index < 0)
            setGpuOperationError(
                "gpu_load_i32 requires a non-negative index",
                LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (state().cuda) {
        int32_t value = 0;
        const auto address = asDevicePointer(buffer.data) +
            static_cast<CUdeviceptr>(index) * sizeof(int32_t);
        checkCuda("cuMemcpyDtoH", state().api.memcpyDtoH(&value, address, sizeof(value)));
        return value;
    }
    if (state().rocm) {
        int32_t value = 0;
        auto* address = static_cast<int32_t*>(buffer.data) + index;
        checkHip("hipMemcpy(DeviceToHost)", state().hip.memcpy(
            &value, address, sizeof(value), hipMemcpyDeviceToHost));
        return value;
    }
    return static_cast<int32_t*>(buffer.data)[index];
}

void rt_gpu_store_i32(void* data, size_t length, int32_t index,
                      int32_t value) {
    const LunaDeviceBufferI32V1 buffer{data, length};
    if (!rt_gpu_initialize() || !data) return;
    if (index < 0 ||
        !validateGpuRange(buffer, static_cast<size_t>(index), 1,
                          "gpu_store_i32")) {
        if (index < 0)
            setGpuOperationError(
                "gpu_store_i32 requires a non-negative index",
                LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return;
    }
    if (state().cuda) {
        const auto address = asDevicePointer(buffer.data) +
            static_cast<CUdeviceptr>(index) * sizeof(int32_t);
        checkCuda("cuMemcpyHtoD", state().api.memcpyHtoD(address, &value, sizeof(value)));
        return;
    }
    if (state().rocm) {
        auto* address = static_cast<int32_t*>(buffer.data) + index;
        checkHip("hipMemcpy(HostToDevice)", state().hip.memcpy(
            address, &value, sizeof(value), hipMemcpyHostToDevice));
        return;
    }
    static_cast<int32_t*>(buffer.data)[index] = value;
}

int rt_gpu_copy_from_host_i32(void* destinationData,
                              size_t destinationLength,
                              const int32_t* source,
                              int32_t element_count) {
    const LunaDeviceBufferI32V1 destination{
        destinationData, destinationLength};
    if (!rt_gpu_initialize()) return 0;
    if (!destination.data || !source) {
        setGpuOperationError(
            "host-to-device copy requires non-null source and destination pointers",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (element_count < 0) {
        setGpuOperationError(
            "host-to-device copy requires a non-negative element count",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (!validateGpuRange(destination, 0,
                          static_cast<size_t>(element_count),
                          "gpu_copy_from_host_i32"))
        return 0;
    const size_t bytes = static_cast<size_t>(element_count) * sizeof(int32_t);
    if (state().cuda)
        return checkCuda("cuMemcpyHtoD(batch)", state().api.memcpyHtoD(
            asDevicePointer(destination.data), source, bytes)) ? 1 : 0;
    if (state().rocm)
        return checkHip("hipMemcpy(HostToDevice, batch)", state().hip.memcpy(
            destination.data, source, bytes, hipMemcpyHostToDevice)) ? 1 : 0;
    std::memcpy(destination.data, source, bytes);
    return 1;
}

int rt_gpu_copy_to_host_i32(int32_t* destination, const void* sourceData,
                            size_t sourceLength,
                            int32_t element_count) {
    const LunaDeviceBufferI32V1 source{
        const_cast<void*>(sourceData), sourceLength};
    if (!rt_gpu_initialize()) return 0;
    if (!destination || !source.data) {
        setGpuOperationError(
            "device-to-host copy requires non-null source and destination pointers",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (element_count < 0) {
        setGpuOperationError(
            "device-to-host copy requires a non-negative element count",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (!validateGpuRange(source, 0,
                          static_cast<size_t>(element_count),
                          "gpu_copy_to_host_i32"))
        return 0;
    const size_t bytes = static_cast<size_t>(element_count) * sizeof(int32_t);
    if (state().cuda)
        return checkCuda("cuMemcpyDtoH(batch)", state().api.memcpyDtoH(
            destination, asDevicePointer(source.data), bytes)) ? 1 : 0;
    if (state().rocm)
        return checkHip("hipMemcpy(DeviceToHost, batch)", state().hip.memcpy(
            destination, source.data, bytes, hipMemcpyDeviceToHost)) ? 1 : 0;
    std::memcpy(destination, source.data, bytes);
    return 1;
}

int32_t rt_gpu_launch_ptx(const char* ptx, const char* kernel_name,
                          int32_t threads, void** params) {
    if (!rt_gpu_initialize()) return 0;
    if (!state().cuda) {
        setGpuOperationError("CUDA launch requested while the active backend is '" +
                             state().backend + "'");
        return 0;
    }
    if (!ptx || !*ptx || !kernel_name || !*kernel_name) {
        setGpuOperationError(
            "CUDA kernel '" + std::string(kernel_name ? kernel_name : "<unknown>") +
                "' has no embedded PTX module",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (threads <= 0) {
        setGpuOperationError(
            "CUDA kernel launch requires a positive thread count",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    auto& runtime = state();
    const std::string moduleKey(ptx ? ptx : "");
    CUmodule module = nullptr;
    auto existing = runtime.modules.find(moduleKey);
    if (existing != runtime.modules.end()) {
        module = existing->second;
    } else {
        if (!checkCuda("cuModuleLoadData", runtime.api.moduleLoadData(&module, ptx))) return 0;
        runtime.modules.emplace(moduleKey, module);
    }
    const std::string functionKey = kernelFunctionCacheKey(moduleKey, kernel_name);
    CUfunction kernel = nullptr;
    auto function = runtime.functions.find(functionKey);
    if (function != runtime.functions.end()) {
        kernel = function->second;
    } else {
        if (!checkCuda("cuModuleGetFunction", runtime.api.moduleGetFunction(&kernel, module, kernel_name)))
            return 0;
        runtime.functions.emplace(functionKey, kernel);
    }
    constexpr unsigned int blockSize = 256;
    const unsigned int blocks = (static_cast<unsigned int>(threads) + blockSize - 1) / blockSize;
    CUevent start = nullptr;
    if (runtime.profileEnabled) {
        if (!checkCuda("cuEventCreate(profile start)", runtime.api.eventCreate(&start, 0)) ||
            !checkCuda("cuEventRecord(profile start)", runtime.api.eventRecord(start, nullptr))) {
            if (start) runtime.api.eventDestroy(start);
            return 0;
        }
    }
    if (!checkCuda("cuLaunchKernel", runtime.api.launchKernel(
            kernel, blocks, 1, 1, blockSize, 1, 1, 0, nullptr, params, nullptr))) {
        if (start) runtime.api.eventDestroy(start);
        return 0;
    }
    CUevent event = nullptr;
    if (!checkCuda("cuEventCreate", runtime.api.eventCreate(&event, 0))) {
        if (start) runtime.api.eventDestroy(start);
        return 0;
    }
    if (!checkCuda("cuEventRecord", runtime.api.eventRecord(event, nullptr))) {
        if (start) runtime.api.eventDestroy(start);
        runtime.api.eventDestroy(event);
        return 0;
    }
    const int32_t handle = runtime.nextEvent++;
    runtime.events.emplace(handle, CudaEventRecord{start, event});
    return handle;
}

int32_t rt_gpu_launch_hsaco(const void* hsaco, size_t hsaco_size,
                            const char* kernel_name, int32_t threads,
                            void** params) {
    if (!rt_gpu_initialize()) return 0;
    if (!state().rocm) {
        setGpuOperationError("ROCm launch requested while the active backend is '" +
                             state().backend + "'");
        return 0;
    }
    if (!hsaco || hsaco_size == 0 || !kernel_name || !*kernel_name) {
        setGpuOperationError(
            "ROCm kernel '" + std::string(kernel_name ? kernel_name : "<unknown>") +
                "' has no embedded HSACO module",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    if (threads <= 0) {
        setGpuOperationError(
            "ROCm kernel launch requires a positive thread count",
            LUNA_RUNTIME_ERROR_INVALID_ARGUMENT);
        return 0;
    }
    auto& runtime = state();
    const std::string moduleKey(static_cast<const char*>(hsaco), hsaco_size);
    hipModule_t module = nullptr;
    auto existing = runtime.hipModules.find(moduleKey);
    if (existing != runtime.hipModules.end()) {
        module = existing->second;
    } else {
        if (!checkHip("hipModuleLoadData", runtime.hip.moduleLoadData(&module, hsaco))) return 0;
        runtime.hipModules.emplace(moduleKey, module);
    }
    const std::string functionKey = kernelFunctionCacheKey(moduleKey, kernel_name);
    hipFunction_t kernel = nullptr;
    auto function = runtime.hipFunctions.find(functionKey);
    if (function != runtime.hipFunctions.end()) {
        kernel = function->second;
    } else {
        if (!checkHip("hipModuleGetFunction", runtime.hip.moduleGetFunction(
                &kernel, module, kernel_name))) return 0;
        runtime.hipFunctions.emplace(functionKey, kernel);
    }
    constexpr unsigned int blockSize = 256;
    const unsigned int blocks = (static_cast<unsigned int>(threads) + blockSize - 1) / blockSize;
    hipEvent_t start = nullptr;
    if (runtime.profileEnabled) {
        if (!checkHip("hipEventCreate(profile start)", runtime.hip.eventCreate(&start)) ||
            !checkHip("hipEventRecord(profile start)", runtime.hip.eventRecord(start, nullptr))) {
            if (start) runtime.hip.eventDestroy(start);
            return 0;
        }
    }
    if (!checkHip("hipModuleLaunchKernel", runtime.hip.launchKernel(
            kernel, blocks, 1, 1, blockSize, 1, 1, 0, nullptr, params, nullptr))) {
        if (start) runtime.hip.eventDestroy(start);
        return 0;
    }
    hipEvent_t event = nullptr;
    if (!checkHip("hipEventCreate", runtime.hip.eventCreate(&event))) {
        if (start) runtime.hip.eventDestroy(start);
        return 0;
    }
    if (!checkHip("hipEventRecord", runtime.hip.eventRecord(event, nullptr))) {
        if (start) runtime.hip.eventDestroy(start);
        runtime.hip.eventDestroy(event);
        return 0;
    }
    const int32_t handle = runtime.nextEvent++;
    runtime.hipEvents.emplace(handle, HipPendingEvent{start, event});
    return handle;
}

int rt_gpu_await_event(int32_t event) {
    if (!rt_gpu_initialize()) return 0;
    if (event == 0) {
        if (state().error.empty())
            setGpuOperationError("launch did not create an event");
        return 0;
    }
    // The simulator completes dispatch before returning its distinguished
    // event handle, while retaining the same source-level await discipline.
    if (!state().cuda && !state().rocm) return event == 1 ? 1 : 0;
    if (state().cuda) {
        auto found = state().events.find(event);
        if (found == state().events.end()) {
            setGpuOperationError("CUDA event handle " + std::to_string(event) + " is invalid or already awaited");
            return 0;
        }
        auto& record = found->second;
        const bool synchronized = checkCuda(
            "cuEventSynchronize", state().api.eventSynchronize(record.completion));
        bool measured = true;
        if (synchronized && record.start) {
            float elapsedMs = 0.0F;
            measured = checkCuda("cuEventElapsedTime", state().api.eventElapsedTime(
                &elapsedMs, record.start, record.completion));
            if (measured) state().profiledKernelMs += elapsedMs;
        }
        const bool startDestroyed = !record.start || checkCuda(
            "cuEventDestroy(profile start)", state().api.eventDestroy(record.start));
        const bool destroyed = checkCuda(
            "cuEventDestroy", state().api.eventDestroy(record.completion));
        state().events.erase(found);
        return synchronized && measured && startDestroyed && destroyed ? 1 : 0;
    } else if (state().rocm) {
        auto found = state().hipEvents.find(event);
        if (found == state().hipEvents.end()) {
            setGpuOperationError("ROCm event handle " + std::to_string(event) + " is invalid or already awaited");
            return 0;
        }
        auto& record = found->second;
        const bool synchronized = checkHip(
            "hipEventSynchronize", state().hip.eventSynchronize(record.completion));
        bool measured = true;
        if (synchronized && record.start) {
            float elapsedMs = 0.0F;
            measured = checkHip("hipEventElapsedTime", state().hip.eventElapsedTime(
                &elapsedMs, record.start, record.completion));
            if (measured) state().profiledKernelMs += elapsedMs;
        }
        const bool startDestroyed = !record.start || checkHip(
            "hipEventDestroy(profile start)", state().hip.eventDestroy(record.start));
        const bool destroyed = checkHip(
            "hipEventDestroy", state().hip.eventDestroy(record.completion));
        state().hipEvents.erase(found);
        return synchronized && measured && startDestroyed && destroyed ? 1 : 0;
    }
    setGpuOperationError("GPU event has no active backend");
    return 0;
}
