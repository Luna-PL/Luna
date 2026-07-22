#pragma once

#include <cstddef>
#include <cstdint>
#include "FragmentPluginABI.h"

#ifdef __cplusplus
extern "C" {
#endif

void* rt_malloc(size_t size);
void  rt_free(void* ptr);
// Language-level printing uses a stable Luna ABI instead of resolving the
// platform's variadic printf from a JIT object. This keeps JIT/AOT output and
// buffering behavior identical across ELF, Mach-O, and MinGW/UCRT.
void  rt_print_i32(int32_t value);
void  rt_print_cstr(const char* value);
// Generated safe array accesses call this before forming a GEP. It never
// returns on failure, preventing undefined behaviour from out-of-bounds IR.
int32_t rt_array_index_or_abort(int32_t index, size_t length);

// Dynamic fragment dispatch is host-only. A generated dynamic apply asks the
// runtime for LUNA_FRAGMENT_<SLOT>; the selected name must be one of the
// compiler-checked candidates embedded by that apply site.
const char* rt_dynamic_fragment_select(const char* slot_name, const char* fallback_name);
int         rt_dynamic_fragment_matches(const char* selected_name, const char* candidate_name);
void        rt_dynamic_fragment_report_unknown_and_abort(const char* slot_name,
                                                         const char* selected_name);

// External fragment plugins are loaded by an explicit host action or by the
// LUNA_FRAGMENT_PLUGIN environment variable.  Loading retains the shared
// library for the process lifetime; callers never receive a native loader
// handle (`dlopen` on POSIX or `LoadLibrary` on Windows).  A successful load
// validates the descriptor before registering it.
int         rt_fragment_plugin_load(const char* path);
const char* rt_fragment_plugin_last_error();
int         rt_fragment_plugin_is_registered(const char* slot_name,
                                              const char* fragment_name,
                                              const char* contract_hash);
int         rt_fragment_plugin_invoke(const char* slot_name,
                                      const char* fragment_name,
                                      const char* contract_hash,
                                      const LunaFragmentInvocationV1* invocation);
void        rt_fragment_plugin_report_error_and_abort();

// The execution backend is selected at runtime through LUNA_GPU_BACKEND:
// `sim` (the default), `cuda`, or `rocm`. Device code-object targets are
// selected separately with the compiler's --gpu-target option. Vendor
// runtimes are loaded dynamically, so building Luna does not require CUDA or
// ROCm SDK headers.
// Set LUNA_GPU_PROFILE=1 to emit accumulated CUDA/ROCm device-event kernel
// time as `Luna GPU profile: kernel_ms=<value>` when the process exits.
int         rt_gpu_initialize();
const char* rt_gpu_backend_name();
const char* rt_gpu_last_error();
void        rt_gpu_report_initialization_error();
// Prints the last launch/synchronization error and terminates the process
// with a non-zero status.  Generated `await` failure edges call this helper;
// it never returns.
void        rt_gpu_report_operation_error_and_abort();
int         rt_gpu_backend_is_cuda();
int         rt_gpu_backend_is_rocm();

// Device memory and host/device scalar transfer. In the simulator a device
// buffer is host-backed; CUDA and ROCm backends keep an opaque device pointer.
void* rt_gpu_alloc_i32(size_t element_count);
void  rt_gpu_free(void* buffer);
int32_t rt_gpu_load_i32(void* buffer, int32_t index);
void    rt_gpu_store_i32(void* buffer, int32_t index, int32_t value);
// Bulk copies use an explicit host raw pointer and element count. They return
// zero on failure so generated host code can enter the same observable error
// boundary used by await.
int rt_gpu_copy_from_host_i32(void* destination, const int32_t* source,
                              int32_t element_count);
int rt_gpu_copy_to_host_i32(int32_t* destination, const void* source,
                            int32_t element_count);

// Launch LLVM-emitted device modules. `params` follows the CUDA Driver / HIP
// Module ABI: each entry points at one host-side parameter value. The returned
// A zero handle denotes a failed launch.  A nonzero event handle is consumed
// by rt_gpu_await_event, which returns 1 on completion and 0 on failure.
int32_t rt_gpu_launch_ptx(const char* ptx, const char* kernel_name,
                          int32_t threads, void** params);
int32_t rt_gpu_launch_hsaco(const void* hsaco, size_t hsaco_size,
                            const char* kernel_name, int32_t threads,
                            void** params);
int     rt_gpu_await_event(int32_t event);

#ifdef __cplusplus
}
#endif
