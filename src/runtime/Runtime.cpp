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

const LunaHostServicesV1 applicationHostServices{
    LUNA_HOST_SERVICES_MAGIC_V1,
    LUNA_RUNTIME_ABI_V1,
    sizeof(LunaHostServicesV1),
    0,
    LUNA_HOST_CAP_ALLOCATOR | LUNA_HOST_CAP_CONSOLE |
        LUNA_HOST_CAP_CONSOLE_INPUT | LUNA_HOST_CAP_FILESYSTEM,
    &defaultAllocator,
    lunaApplicationConsoleV1(),
    nullptr,
    lunaApplicationFileSystemV1(),
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

int rt_install_application_host_services_v1() {
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
    installedHostServices.store(&applicationHostServices,
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

void* rt_gpu_alloc_i32(size_t element_count) {
    if (!rt_gpu_initialize()) return nullptr;
    if (state().cuda) {
        CUdeviceptr buffer = 0;
        if (!checkCuda("cuMemAlloc", state().api.memAlloc(&buffer, element_count * sizeof(int32_t))))
            return nullptr;
        return asOpaquePointer(buffer);
    }
    if (state().rocm) {
        void* buffer = nullptr;
        if (!checkHip("hipMalloc", state().hip.malloc(&buffer, element_count * sizeof(int32_t))))
            return nullptr;
        return buffer;
    }
    return std::calloc(element_count, sizeof(int));
}

void rt_gpu_free(void* buffer) {
    if (!rt_gpu_initialize() || !buffer) return;
    if (state().cuda) {
        checkCuda("cuMemFree", state().api.memFree(asDevicePointer(buffer)));
        return;
    }
    if (state().rocm) {
        checkHip("hipFree", state().hip.free(buffer));
        return;
    }
    std::free(buffer);
}

int32_t rt_gpu_load_i32(void* buffer, int32_t index) {
    if (!rt_gpu_initialize() || !buffer) return 0;
    if (state().cuda) {
        int32_t value = 0;
        const auto address = asDevicePointer(buffer) +
            static_cast<CUdeviceptr>(index) * sizeof(int32_t);
        checkCuda("cuMemcpyDtoH", state().api.memcpyDtoH(&value, address, sizeof(value)));
        return value;
    }
    if (state().rocm) {
        int32_t value = 0;
        auto* address = static_cast<int32_t*>(buffer) + index;
        checkHip("hipMemcpy(DeviceToHost)", state().hip.memcpy(
            &value, address, sizeof(value), hipMemcpyDeviceToHost));
        return value;
    }
    return static_cast<int32_t*>(buffer)[index];
}

void rt_gpu_store_i32(void* buffer, int32_t index, int32_t value) {
    if (!rt_gpu_initialize() || !buffer) return;
    if (state().cuda) {
        const auto address = asDevicePointer(buffer) +
            static_cast<CUdeviceptr>(index) * sizeof(int32_t);
        checkCuda("cuMemcpyHtoD", state().api.memcpyHtoD(address, &value, sizeof(value)));
        return;
    }
    if (state().rocm) {
        auto* address = static_cast<int32_t*>(buffer) + index;
        checkHip("hipMemcpy(HostToDevice)", state().hip.memcpy(
            address, &value, sizeof(value), hipMemcpyHostToDevice));
        return;
    }
    static_cast<int32_t*>(buffer)[index] = value;
}

int rt_gpu_copy_from_host_i32(void* destination, const int32_t* source,
                              int32_t element_count) {
    if (!rt_gpu_initialize()) return 0;
    if (!destination || !source) {
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
    const size_t bytes = static_cast<size_t>(element_count) * sizeof(int32_t);
    if (state().cuda)
        return checkCuda("cuMemcpyHtoD(batch)", state().api.memcpyHtoD(
            asDevicePointer(destination), source, bytes)) ? 1 : 0;
    if (state().rocm)
        return checkHip("hipMemcpy(HostToDevice, batch)", state().hip.memcpy(
            destination, source, bytes, hipMemcpyHostToDevice)) ? 1 : 0;
    std::memcpy(destination, source, bytes);
    return 1;
}

int rt_gpu_copy_to_host_i32(int32_t* destination, const void* source,
                            int32_t element_count) {
    if (!rt_gpu_initialize()) return 0;
    if (!destination || !source) {
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
    const size_t bytes = static_cast<size_t>(element_count) * sizeof(int32_t);
    if (state().cuda)
        return checkCuda("cuMemcpyDtoH(batch)", state().api.memcpyDtoH(
            destination, asDevicePointer(const_cast<void*>(source)), bytes)) ? 1 : 0;
    if (state().rocm)
        return checkHip("hipMemcpy(DeviceToHost, batch)", state().hip.memcpy(
            destination, const_cast<void*>(source), bytes, hipMemcpyDeviceToHost)) ? 1 : 0;
    std::memcpy(destination, source, bytes);
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
