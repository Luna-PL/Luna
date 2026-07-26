#include "Runtime.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

struct SharedAllocationHeader {
    void* allocation = nullptr;
    size_t allocationSize = 0;
    size_t allocationAlignment = 0;
    uint64_t rcCount = 1;
    std::atomic<uint64_t> arcCount{1};
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
    return ::operator new(actualSize, std::align_val_t(alignment), std::nothrow);
}

void defaultDeallocate(void*, void* pointer, size_t, size_t alignment) {
    if (!pointer) return;
    if (alignment <= alignof(std::max_align_t)) {
        std::free(pointer);
        return;
    }
    ::operator delete(pointer, std::align_val_t(alignment));
}

void* defaultReallocate(void* context, void* pointer, size_t oldSize,
                        size_t newSize, size_t alignment) {
    if (!isValidAlignment(alignment)) return nullptr;
    if (!pointer) return defaultAllocate(context, newSize, alignment);
    if (newSize == 0) {
        defaultDeallocate(context, pointer, oldSize, alignment);
        return nullptr;
    }
    if (alignment <= alignof(std::max_align_t)) return std::realloc(pointer, newSize);
    void* replacement = defaultAllocate(context, newSize, alignment);
    if (!replacement) return nullptr;
    std::memcpy(replacement, pointer, std::min(oldSize, newSize));
    defaultDeallocate(context, pointer, oldSize, alignment);
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
};

std::atomic<const LunaHostServicesV1*> installedHostServices{&defaultHostServices};
// 0: configurable, -1: an installation is in progress, 1: services are live.
std::atomic<int> hostServicesPhase{0};

bool validAllocator(const LunaAllocatorV1* allocator) {
    return allocator && allocator->abi_version == LUNA_RUNTIME_ABI_V1 &&
        allocator->struct_size >= sizeof(LunaAllocatorV1) &&
        allocator->allocate && allocator->reallocate && allocator->deallocate;
}

bool validConsole(const LunaConsoleV1* console) {
    return console && console->abi_version == LUNA_RUNTIME_ABI_V1 &&
        console->struct_size >= sizeof(LunaConsoleV1) &&
        console->write && console->flush;
}

bool validExecutableMemory(const LunaExecutableMemoryV1* memory) {
    return memory && memory->abi_version == LUNA_RUNTIME_ABI_V1 &&
        memory->struct_size >= sizeof(LunaExecutableMemoryV1) &&
        memory->reserve && memory->seal && memory->release;
}

bool validHostServices(const LunaHostServicesV1* services) {
    if (!services || services->magic != LUNA_HOST_SERVICES_MAGIC_V1 ||
        services->abi_version != LUNA_RUNTIME_ABI_V1 ||
        services->struct_size < sizeof(LunaHostServicesV1) ||
        services->reserved_zero != 0 ||
        (services->capabilities & LUNA_HOST_CAP_ALLOCATOR) == 0 ||
        !validAllocator(services->allocator))
        return false;
    if ((services->capabilities & LUNA_HOST_CAP_CONSOLE) != 0 &&
        !validConsole(services->console))
        return false;
    if ((services->capabilities & LUNA_HOST_CAP_EXECUTABLE_MEMORY) != 0 &&
        !validExecutableMemory(services->executable_memory))
        return false;
    return true;
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
    bool cuda = false;
    bool rocm = false;
    std::string backend = "sim";
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

struct FragmentPluginState {
    struct Loaded {
        void* library = nullptr;
        const LunaFragmentPluginDescriptorV1* descriptor = nullptr;
        std::string path;
    };
    std::vector<Loaded> loaded;
    std::string error;
};

GpuRuntimeState& state() {
    static GpuRuntimeState value;
    return value;
}

FragmentPluginState& fragmentPlugins() {
    static FragmentPluginState value;
    return value;
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

std::string dynamicFragmentEnvironmentKey(const char* slotName) {
    std::string key = "LUNA_FRAGMENT_";
    for (const char* character = slotName; character && *character; ++character) {
        const unsigned char value = static_cast<unsigned char>(*character);
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'))
            key.push_back(static_cast<char>(std::toupper(value)));
        else if ((value >= '0' && value <= '9') || value == '_')
            key.push_back(static_cast<char>(value));
        else
            key.push_back('_');
    }
    return key;
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
    state().error = std::string("CUDA Driver API call '") + operation +
        "' failed with code " + std::to_string(status);
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
        runtime.error = "could not load " + std::string(libraryName) + ": " +
            lunaDynamicLoaderError();
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
        runtime.error = std::string(libraryName) +
            " is missing a required CUDA Driver API symbol";
        lunaCloseLibrary(api.library);
        api.library = nullptr;
        return false;
    }
    return true;
}

void setHipError(const char* operation, hipError_t status) {
    state().error = std::string("HIP runtime call '") + operation +
        "' failed with code " + std::to_string(status);
    if (state().hip.getErrorString) {
        const char* description = state().hip.getErrorString(status);
        if (description && *description)
            state().error += " (" + std::string(description) + ")";
    }
}

bool checkHip(const char* operation, hipError_t status) {
    if (status == hipSuccess) return true;
    setHipError(operation, status);
    return false;
}

void setGpuOperationError(const std::string& message) {
    state().error = message;
}

const LunaFragmentPluginDescriptorV1* findFragmentPlugin(
    const char* slotName, const char* fragmentName, const char* contractHash) {
    if (!slotName || !fragmentName || !contractHash) return nullptr;
    for (const auto& loaded : fragmentPlugins().loaded) {
        const auto* descriptor = loaded.descriptor;
        if (!descriptor || !descriptor->slot_name || !descriptor->fragment_name ||
            !descriptor->contract_hash)
            continue;
        if (std::strcmp(descriptor->slot_name, slotName) == 0 &&
            std::strcmp(descriptor->fragment_name, fragmentName) == 0 &&
            std::strcmp(descriptor->contract_hash, contractHash) == 0)
            return descriptor;
    }
    return nullptr;
}

bool validateFragmentPluginDescriptor(
    const LunaFragmentPluginDescriptorV1* descriptor, const char* path) {
    auto& plugins = fragmentPlugins();
    const std::string source = path ? path : "<plugin>";
    if (!descriptor) {
        plugins.error = "plugin '" + source + "' returned a null descriptor";
        return false;
    }
    if (descriptor->magic != LUNA_FRAGMENT_PLUGIN_DESCRIPTOR_V1 ||
        descriptor->abi_version != LUNA_FRAGMENT_PLUGIN_ABI_V1) {
        plugins.error = "plugin '" + source + "' uses an unsupported fragment ABI";
        return false;
    }
    if (descriptor->descriptor_size < sizeof(LunaFragmentPluginDescriptorV1)) {
        plugins.error = "plugin '" + source + "' has a truncated fragment descriptor";
        return false;
    }
    if (!descriptor->plugin_id || !*descriptor->plugin_id ||
        !descriptor->fragment_name || !*descriptor->fragment_name ||
        !descriptor->slot_name || !*descriptor->slot_name ||
        !descriptor->contract_hash || !*descriptor->contract_hash ||
        !descriptor->entry) {
        plugins.error = "plugin '" + source +
            "' has incomplete fragment metadata or no entry point";
        return false;
    }
    if (descriptor->fragment_kind != LUNA_FRAGMENT_KIND_INTERCEPTOR ||
        descriptor->cardinality != LUNA_FRAGMENT_CARDINALITY_ONCE ||
        (descriptor->effects & LUNA_FRAGMENT_EFFECT_HOST_ONLY) == 0 ||
        (descriptor->effects & ~(LUNA_FRAGMENT_EFFECT_HOST_ONLY |
                                 LUNA_FRAGMENT_EFFECT_MAY_ABORT)) != 0) {
        plugins.error = "plugin '" + source +
            "' is outside the v1 external fragment contract: only host-only "
            "single-shot interceptors are supported";
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
        runtime.error = "could not load HIP runtime library (install the ROCm HIP runtime): " +
            lunaDynamicLoaderError();
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
        runtime.error = "HIP runtime library is missing a required HIP Module API symbol";
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

const LunaHostServicesV1* rt_host_services_v1() {
    return activateHostServices();
}

void* rt_alloc(size_t size, size_t alignment) {
    if (!isValidAlignment(alignment)) return nullptr;
    const auto* services = activateHostServices();
    return services->allocator->allocate(
        services->allocator->context, size, alignment);
}

void* rt_realloc(void* pointer, size_t old_size, size_t new_size,
                 size_t alignment) {
    if (!isValidAlignment(alignment)) return nullptr;
    const auto* services = activateHostServices();
    const auto* allocator = services->allocator;
    if (!allocator->reallocate) return nullptr;
    return allocator->reallocate(
        allocator->context, pointer, old_size, new_size, alignment);
}

void rt_dealloc(void* pointer, size_t size, size_t alignment) {
    if (!pointer || !isValidAlignment(alignment)) return;
    const auto* services = activateHostServices();
    services->allocator->deallocate(
        services->allocator->context, pointer, size, alignment);
}

static void* sharedAlloc(size_t size, size_t alignment) {
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
    return reinterpret_cast<void*>(payload);
}

void* rt_rc_alloc(size_t size, size_t alignment) {
    return sharedAlloc(size, alignment);
}

void rt_rc_retain(void* pointer) {
    if (pointer) ++sharedHeader(pointer)->rcCount;
}

int32_t rt_rc_release(void* pointer) {
    if (!pointer) return 0;
    auto* header = sharedHeader(pointer);
    if (header->rcCount == 0) return 0;
    return --header->rcCount == 0 ? 1 : 0;
}

void* rt_arc_alloc(size_t size, size_t alignment) {
    return sharedAlloc(size, alignment);
}

void rt_arc_retain(void* pointer) {
    if (pointer)
        sharedHeader(pointer)->arcCount.fetch_add(
            1, std::memory_order_relaxed);
}

int32_t rt_arc_release(void* pointer) {
    if (!pointer) return 0;
    return sharedHeader(pointer)->arcCount.fetch_sub(
        1, std::memory_order_acq_rel) == 1 ? 1 : 0;
}

void rt_shared_dealloc(void* pointer) {
    if (!pointer) return;
    auto* header = sharedHeader(pointer);
    void* allocation = header->allocation;
    const size_t size = header->allocationSize;
    const size_t alignment = header->allocationAlignment;
    header->~SharedAllocationHeader();
    rt_dealloc(allocation, size, alignment);
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

void* rt_malloc(size_t size) {
    return rt_alloc(size, LUNA_DEFAULT_HOST_ALIGNMENT);
}

void rt_free(void* ptr) {
    rt_dealloc(ptr, 0, LUNA_DEFAULT_HOST_ALIGNMENT);
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

int32_t rt_array_index_or_abort(int32_t index, size_t length) {
    if (index >= 0 && static_cast<size_t>(index) < length) return index;
    std::fprintf(stderr, "Luna runtime error: array index %d is outside length %zu\n", index, length);
    std::abort();
}

const char* rt_dynamic_fragment_select(const char* slot_name, const char* fallback_name) {
    if (const char* plugin = std::getenv("LUNA_FRAGMENT_PLUGIN"); plugin && *plugin)
        rt_fragment_plugin_load(plugin);
    const std::string key = dynamicFragmentEnvironmentKey(slot_name);
    const char* selected = std::getenv(key.c_str());
    return selected && *selected ? selected : fallback_name;
}

int rt_dynamic_fragment_matches(const char* selected_name, const char* candidate_name) {
    return selected_name && candidate_name && std::strcmp(selected_name, candidate_name) == 0;
}

void rt_dynamic_fragment_report_unknown_and_abort(const char* slot_name,
                                                  const char* selected_name) {
    std::fprintf(stderr, "dynamic fragment selection '%s' is not registered for slot '%s'\n",
                 selected_name ? selected_name : "<null>",
                 slot_name ? slot_name : "<unknown>");
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

int rt_fragment_plugin_load(const char* path) {
    auto& plugins = fragmentPlugins();
    plugins.error.clear();
    if (!path || !*path) {
        plugins.error = "fragment plugin path is empty";
        return 0;
    }

    for (const auto& loaded : plugins.loaded) {
        if (loaded.path == path) return 1;
    }

    void* library = lunaOpenLibrary(path);
    if (!library) {
        plugins.error = "could not load fragment plugin '" + std::string(path) + "': " +
            lunaDynamicLoaderError();
        return 0;
    }

    auto descriptorFn = reinterpret_cast<LunaFragmentPluginDescriptorFnV1>(
        lunaLoadSymbol(library, "luna_fragment_plugin_descriptor_v1"));
    if (!descriptorFn) {
        plugins.error = "fragment plugin '" + std::string(path) +
            "' does not export luna_fragment_plugin_descriptor_v1";
        lunaCloseLibrary(library);
        return 0;
    }

    const auto* descriptor = descriptorFn();
    if (!validateFragmentPluginDescriptor(descriptor, path)) {
        lunaCloseLibrary(library);
        return 0;
    }
    if (findFragmentPlugin(descriptor->slot_name, descriptor->fragment_name,
                           descriptor->contract_hash)) {
        plugins.error = "fragment plugin '" + std::string(path) +
            "' duplicates registered fragment '" + descriptor->fragment_name +
            "' for slot '" + descriptor->slot_name + "'";
        lunaCloseLibrary(library);
        return 0;
    }

    plugins.loaded.push_back({library, descriptor, path});
    return 1;
}

const char* rt_fragment_plugin_last_error() {
    return fragmentPlugins().error.c_str();
}

int rt_fragment_plugin_is_registered(const char* slot_name,
                                     const char* fragment_name,
                                     const char* contract_hash) {
    return findFragmentPlugin(slot_name, fragment_name, contract_hash) ? 1 : 0;
}

int rt_fragment_plugin_invoke(const char* slot_name,
                              const char* fragment_name,
                              const char* contract_hash,
                              const LunaFragmentInvocationV1* invocation) {
    auto* descriptor = findFragmentPlugin(slot_name, fragment_name, contract_hash);
    if (!descriptor) {
        fragmentPlugins().error = "no external fragment registered for slot '" +
            std::string(slot_name ? slot_name : "<unknown>") + "', fragment '" +
            std::string(fragment_name ? fragment_name : "<unknown>") +
            "' and the requested contract";
        return LUNA_FRAGMENT_PLUGIN_ERROR;
    }
    if (!invocation || invocation->abi_version != LUNA_FRAGMENT_PLUGIN_ABI_V1) {
        fragmentPlugins().error = "external fragment invocation uses an unsupported ABI";
        return LUNA_FRAGMENT_PLUGIN_ERROR;
    }
    const int result = descriptor->entry(invocation);
    if (result != LUNA_FRAGMENT_PLUGIN_CONTINUE &&
        result != LUNA_FRAGMENT_PLUGIN_ABORT) {
        fragmentPlugins().error = "external fragment '" +
            std::string(descriptor->fragment_name) + "' returned an invalid action";
        return LUNA_FRAGMENT_PLUGIN_ERROR;
    }
    if (result == LUNA_FRAGMENT_PLUGIN_ABORT &&
        (descriptor->effects & LUNA_FRAGMENT_EFFECT_MAY_ABORT) == 0) {
        fragmentPlugins().error = "external fragment '" +
            std::string(descriptor->fragment_name) +
            "' returned abort without declaring the may-abort effect";
        return LUNA_FRAGMENT_PLUGIN_ERROR;
    }
    return result;
}

void rt_fragment_plugin_report_error_and_abort() {
    const char* message = fragmentPlugins().error.empty()
        ? "unknown external fragment plugin failure"
        : fragmentPlugins().error.c_str();
    std::fprintf(stderr, "Luna external fragment plugin error: %s\n", message);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

int rt_gpu_initialize() {
    auto& runtime = state();
    if (runtime.initialized) return runtime.error.empty() ? 1 : 0;
    runtime.initialized = true;
    runtime.profileEnabled = gpuProfilingRequested();
    if (runtime.profileEnabled && !runtime.profileReporterRegistered) {
        std::atexit(reportGpuProfileAtExit);
        runtime.profileReporterRegistered = true;
    }
    const char* requested = std::getenv("LUNA_GPU_BACKEND");
    if (!requested || std::strcmp(requested, "sim") == 0 || std::strcmp(requested, "cpu") == 0)
        return 1;
    runtime.backend = requested;
    if (std::strcmp(requested, "cuda") == 0) {
        runtime.backend = "cuda";
        if (!loadCudaApi()) return 0;
        if (!checkCuda("cuInit", runtime.api.init(0))) return 0;
        CUdevice device = 0;
        if (!checkCuda("cuDeviceGet", runtime.api.deviceGet(&device, 0))) return 0;
        if (!checkCuda("cuCtxCreate", runtime.api.ctxCreate(&runtime.context, 0, device))) return 0;
        runtime.cuda = true;
        return 1;
    }
    if (std::strcmp(requested, "rocm") == 0 || std::strcmp(requested, "hip") == 0) {
        runtime.backend = "rocm";
        if (!loadHipApi()) return 0;
        if (!checkHip("hipInit", runtime.hip.init(0))) return 0;
        if (!checkHip("hipSetDevice", runtime.hip.setDevice(0))) return 0;
        runtime.rocm = true;
        return 1;
    }
    {
        runtime.error = std::string("unknown GPU backend '") + requested +
            "'; use 'sim', 'cuda', or 'rocm'";
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
        setGpuOperationError("host-to-device copy requires non-null source and destination pointers");
        return 0;
    }
    if (element_count < 0) {
        setGpuOperationError("host-to-device copy requires a non-negative element count");
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
        setGpuOperationError("device-to-host copy requires non-null source and destination pointers");
        return 0;
    }
    if (element_count < 0) {
        setGpuOperationError("device-to-host copy requires a non-negative element count");
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
    if (!ptx || !*ptx) {
        setGpuOperationError("CUDA kernel '" + std::string(kernel_name ? kernel_name : "<unknown>") +
                             "' has no embedded PTX module");
        return 0;
    }
    if (threads <= 0) {
        setGpuOperationError("CUDA kernel launch requires a positive thread count");
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
    if (!hsaco || hsaco_size == 0) {
        setGpuOperationError("ROCm kernel '" + std::string(kernel_name ? kernel_name : "<unknown>") +
                             "' has no embedded HSACO module");
        return 0;
    }
    if (threads <= 0) {
        setGpuOperationError("ROCm kernel launch requires a positive thread count");
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
