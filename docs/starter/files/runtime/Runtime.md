# src/runtime/Runtime.cpp

Default implementation of all C ABI entry points of the Luna runtime, covering host service installation, memory management (plain/RC/ARC), console I/O, dynamic loading of GPU backends (CUDA/ROCm), fragment plugin loading, dynamic fragment dispatch, and error snapshots.

## What This File Does

- Implements all `rt_*` functions declared in `Runtime.h`.
- Provides default `LunaHostServicesV1`, `LunaAllocatorV1`, and `LunaConsoleV1` instances (`defaultHostServices`, `defaultAllocator`, `defaultConsole`).
- Provides the application-level host services `applicationHostServices`, including console input and the file system.
- Implements single-shot installation and a three-phase lifecycle for host services via `std::atomic` (0 = configurable, -1 = installing, 1 = active).
- Manages GPU runtime state (`GpuRuntimeState`): function-static local variables within the anonymous namespace hold the CUDA Driver API and HIP API function pointers, which are dynamically loaded (`dlopen`/ `LoadLibrary`) only when the user selects the `cuda`/ `rocm` backend.
- Manages fragment plugin state (`FragmentPluginState`): the list of loaded shared libraries, error codes, and error messages.
- Handles dynamic fragment selection: reads the name from the environment variable `LUNA_FRAGMENT_<SLOT>`.
- Implements GPU profiling (`LUNA_GPU_PROFILE=1`): reports cumulative kernel time via `std::atexit` at process exit.

## Key Structs, Classes, and Enums

### Anonymous namespace class definitions

| Type | Purpose | C++ analog |
|---|---|---|
| `AtomicSharedCounter` | Alias for `std::atomic<uint64_t>` | Atomic integer |
| `SharedCounter` | A `union` that can be interpreted as a plain `uint64_t` (RC) or an `AtomicSharedCounter` (ARC) | Similar to `std::variant<uint64_t, std::atomic<uint64_t>>` |
| `SharedAllocationHeader` | RC/ARC allocation header: allocation base address, size, alignment, count, and `LunaDropCallbackV1` destructor callback | Similar to a `std::shared_ptr` control block |
| `CudaApi` | Collection of CUDA Driver API function pointers (16 function pointers) | Function pointer table |
| `HipApi` | Collection of HIP API function pointers (15 function pointers) | Function pointer table |
| `CudaEventRecord` | CUDA launch/completion event pair | — |
| `HipPendingEvent` | HIP launch/completion event pair | — |
| `GpuRuntimeState` | Global GPU runtime state: initialization flag, backend type, CUDA/HIP handles, module cache, function cache, event cache, cumulative profiling time | Singleton state object |
| `FragmentPluginState::Loaded` | A loaded fragment plugin: shared library pointer, descriptor pointer, path | — |
| `FragmentPluginState` | Fragment plugin runtime state: load list, error code, error message | — |

### Key constants

| Constant | Value | Purpose |
|---|---|---|
| `defaultHostServices` | `LunaHostServicesV1` | Default host services, providing only the allocator + console output |
| `applicationHostServices` | `LunaHostServicesV1` | Application-level host services, additionally providing console input + file system |
| `hostServicesPhase` | `std::atomic<int>` | Three phases: 0 = configurable, -1 = installing, 1 = active |

## Key Functions and Methods

### Host service installation (`activateHostServices` / `validHostServices` and others)

| Function | Purpose |
|---|---|
| `activateHostServices()` | Spin-CAS that advances the phase from 0 to 1 and returns the installed `LunaHostServicesV1*`; similar to one-shot double-checked locking |
| `validHostServices` | Deep validation: the magic field, abi_version, struct_size, reserved_zero, and capabilities must include `LUNA_HOST_CAP_ALLOCATOR`, after which each subtable is validated bit by bit according to the capability bits |
| `validAllocator` / `validConsoleOutput` / `validConsoleInput` / `validExecutableMemory` / `validFileSystem` | Validity-check functions for each subtable |

### Default allocator callbacks

| Function | Purpose |
|---|---|
| `defaultAllocate` | Uses `std::malloc` when alignment `<= alignof(max_align_t)`; for larger alignment uses `posix_memalign` on POSIX and `_aligned_malloc` on Windows |
| `defaultDeallocate` | Strictly paired with the allocation path: `std::free` on POSIX and `_aligned_free` for large alignment on Windows |
| `defaultReallocate` | Uses `std::realloc` for ordinary alignment; for larger alignment allocates new memory, memcpy, and frees the old memory |

### Reference-counted memory

| Function | Purpose |
|---|---|
| `rt_rc_allocate_v1` | Allocates a `SharedAllocationHeader` + data area and returns a data area pointer, with rc initialized to 1 |
| `rt_rc_retain_v1` | Non-atomic increment of rc |
| `rt_rc_release_v1` | Non-atomic decrement of rc; when it reaches zero, calls the drop callback and frees the entire block |
| `rt_arc_allocate_v1` / `rt_arc_retain_v1` / `rt_arc_release_v1` | Same as above, but uses `std::atomic`'s `fetch_add` / `fetch_sub` for cross-thread safety |

### GPU backends

| Function | Purpose |
|---|---|
| `rt_gpu_initialize` | Reads `LUNA_GPU_BACKEND`; if `cuda`, `dlopen("libcuda.so.1")` and loads the 16 CUDA Driver API symbols; if `rocm`, loads `libamdhip64.so` and the 15 HIP symbols |
| `rt_gpu_launch_ptx` | Caches `cuModuleLoadData`/ `cuModuleGetFunction` and calls `cuLaunchKernel` |
| `rt_gpu_launch_hsaco` | Same as above, but uses HIP to load the HSA Code Object |
| `rt_gpu_await_event` | Calls `cuEventSynchronize`/ `hipEventSynchronize` and records the elapsed time into `profiledKernelMs` |
| `checkCuda` / `checkHip` | Error checks after every API call; calls `setGpuError` on failure |
| `reportGpuProfileAtExit` | Registered via `std::atexit`; prints `Luna GPU profile: kernel_ms=...` |

### Dynamic fragment dispatch

| Function | Purpose |
|---|---|
| `dynamicFragmentEnvironmentKey` | Converts slot_name into the fully-uppercased environment variable key `LUNA_FRAGMENT_<SLOT>`, mapping non-alphanumeric, non-underscore characters to `_` |
| `rt_dynamic_fragment_select` | Reads the environment variable; returns fallback_name if it is empty |
| `rt_dynamic_fragment_matches` | Compares selected_name with candidate_name as strings |
| `rt_dynamic_fragment_report_unknown_and_abort` | Prints an error message and calls `std::abort` |

### Fragment plugin loading

| Function | Purpose |
|---|---|
| `rt_fragment_plugin_load` | Calls `lunaOpenLibrary` (`dlopen`/ `LoadLibrary`), looks up the `luna_fragment_plugin_descriptor_v1` symbol, validates the descriptor, and registers it into `FragmentPluginState` |
| `rt_fragment_plugin_is_registered` | Iterates over the loaded list, matching on the slot_name + fragment_name + contract_hash triple |
| `rt_fragment_plugin_invoke` | Finds the matching plugin and invokes its `entry` function pointer |

### Utility functions

| Function | Purpose |
|---|---|
| `lunaOpenLibrary` / `lunaLoadSymbol` / `lunaCloseLibrary` | Cross-platform dynamic library loading adapters wrapping POSIX `dlopen`/ `dlsym`/ `dlclose` and Windows `LoadLibraryA`/ `GetProcAddress`/ `FreeLibrary` |
| `kernelFunctionCacheKey` | Builds a cache key string of module key + null-character separator + kernel name, preventing handle collisions for same-named kernels in different modules |
| `sharedHeader` | Recovers the `SharedAllocationHeader` pointer from an RC/ARC data pointer |

## Relationship to Surrounding Files and Pipeline Stages

- **Runtime.h** — Declarations of all functions implemented by this file. `Runtime.cpp` `#include`s it directly.
- **RuntimeABI.h** — Provides the definitions of structs such as `LunaHostServicesV1` and all the `LUNA_*` constant macros. `Runtime.cpp` `#include`s it directly.
- **ApplicationHostServices.h** — Provides `lunaApplicationConsoleV1` / `lunaApplicationFileSystemV1`, used to construct the `applicationHostServices` constant.
- **FragmentPluginABI.h** — Provides types such as `LunaFragmentPluginDescriptorV1`, used for plugin loading validation.
- **Generated IR** — Compiler-generated code calls the `rt_*` functions implemented by this file at runtime.
- This file does not depend on any Luna compiler internals; it depends only on the standard C/C++ runtime and the platform dynamic-loading APIs.

## Further Reading

- Full semantics of all struct fields in `RuntimeABI.h`
- ABI specification of the fragment plugin descriptor in `FragmentPluginABI.h`
- Concrete implementation of the file system and console input in `ApplicationHostServices.cpp`
- CUDA Driver API documentation (`cuModuleLoadData`/ `cuLaunchKernel`, etc.)
- ROCm HIP API documentation (`hipModuleLoadData`/ `hipModuleLaunchKernel`, etc.)


---

---
title: Runtime.h
source: src/runtime/Runtime.h
language: en
audience: Luna runtime implementers / embedding hosts
---

# src/runtime/Runtime.h

The master declaration header of the Luna runtime's C ABI entry points, defining every externally callable function: host environment installation, memory management, console I/O, GPU backend management, fragment plugin loading and dynamic dispatch, and array bounds checking.

## What This File Does

- Declares the host service installation and query interfaces: `rt_install_host_services_v1`, `rt_install_application_host_services_v1`, `rt_host_services_v1`.
- Declares the Luna managed memory allocators: plain allocation (`rt_alloc`/ `rt_realloc`/ `rt_dealloc`), reference counting RC (`rt_rc_allocate_v1`/ `rt_rc_retain_v1`/ `rt_rc_release_v1`), and atomic reference counting ARC (`rt_arc_allocate_v1`/ `rt_arc_retain_v1`/ `rt_arc_release_v1`).
- Declares console output, input, and formatted-print utility functions (`rt_print_i32`, `rt_print_cstr`).
- Declares the version 0.2 compatibility bridge layer (five functions such as `rt_compat_console_write_cstr_0_2`).
- Declares the GPU backend function cluster: initialization, device memory allocation, data transfer, kernel launch, and event waiting (`rt_gpu_*`).
- Declares fragment dynamic dispatch (`rt_dynamic_fragment_select`/ `rt_dynamic_fragment_matches`/ `rt_dynamic_fragment_report_unknown_and_abort`) and external plugin loading (`rt_fragment_plugin_load`/ `rt_fragment_plugin_invoke`, etc.).
- Declares the array bounds check `rt_array_index_or_abort`.
- Error snapshot query `rt_runtime_error_snapshot_v1`.

The functions are grouped by functionality and separated by standalone block comments, with the whole wrapped in `extern "C"` to guarantee the C linking convention.

## Key Structs, Classes, and Enums

This file defines no structs; all struct definitions live in `RuntimeABI.h`. This file only references the following types in its function signatures:

- `LunaHostServicesV1` — Host service descriptor containing subtables for the allocator, console, executable memory, file system, etc.
- `LunaRuntimeErrorSnapshotV1` — Runtime error snapshot containing domain, code, and message_size.
- `LunaDropCallbackV1` — Destructor callback function pointer type (`void(*)(void* value_storage)`).
- `LunaFragmentInvocationV1` — Fragment invocation argument pack, defined in `FragmentPluginABI.h`.

## Key Functions and Methods

### Host service installation

| Function | Purpose |
|---|---|
| `rt_install_host_services_v1` | Installs a custom host service descriptor; must be called before the first runtime service use; passing `nullptr` is rejected |
| `rt_install_application_host_services_v1` | Installs process-level console input + file system services, for use by ordinary generated Luna application entry points |
| `rt_host_services_v1` | Returns the currently installed `LunaHostServicesV1*` |

### Memory management

| Function | C++ analog |
|---|---|
| `rt_alloc` / `rt_realloc` / `rt_dealloc` | Similar to `::operator new` / `std::realloc` / `::operator delete`, but with explicit alignment |
| `rt_rc_allocate_v1` / `rt_rc_retain_v1` / `rt_rc_release_v1` | A non-atomic reference-counting implementation similar to `std::shared_ptr`; the `LunaDropCallbackV1` destructor callback is passed at allocation time |
| `rt_arc_allocate_v1` / `rt_arc_retain_v1` / `rt_arc_release_v1` | Atomic reference counting (a `std::atomic<int>` version of `shared_ptr`), usable for cross-thread sharing |
| `rt_panic_cstr` | Unrecoverable error: prints a message and terminates the process (similar to `std::terminate` + a custom message) |

### Console I/O and compatibility bridge

| Function | Purpose |
|---|---|
| `rt_print_i32` / `rt_print_cstr` | Language-level integer/string printing using the stable Luna ABI, avoiding the JIT resolving the platform `printf` |
| `rt_compat_console_write_cstr_0_2` and others | Temporary adapter for the version 0.2 standard library, to be replaced by the 0.3 safe I/O layer later |
| `rt_array_index_or_abort` | Called by generated code before GEP; aborts on out-of-bounds access to prevent undefined behavior |

### Fragment dynamic dispatch

| Function | Purpose |
|---|---|
| `rt_dynamic_fragment_select` | Reads the selection from the environment variable `LUNA_FRAGMENT_<SLOT>` based on slot_name |
| `rt_dynamic_fragment_matches` | Checks whether selected_name equals candidate_name |
| `rt_dynamic_fragment_report_unknown_and_abort` | Prints an error and aborts when an unknown fragment is selected |

### External fragment plugins

| Function | Purpose |
|---|---|
| `rt_fragment_plugin_load` | Loads a shared library (`dlopen`/ `LoadLibrary`) and validates the descriptor; resident at the process level |
| `rt_fragment_plugin_last_error` | Returns the string of the most recent load error |
| `rt_fragment_plugin_is_registered` | Queries registration state by the slot_name + fragment_name + contract_hash triple |
| `rt_fragment_plugin_invoke` | Invokes the registered fragment entry point |
| `rt_fragment_plugin_report_error_and_abort` | Reports a plugin error and aborts |

### GPU backend

| Function | Purpose |
|---|---|
| `rt_gpu_initialize` | Initializes the backend according to `LUNA_GPU_BACKEND` (`sim`/ `cuda`/ `rocm`) |
| `rt_gpu_backend_name` / `rt_gpu_backend_is_cuda` / `rt_gpu_backend_is_rocm` | Query the current backend type |
| `rt_gpu_alloc_i32` / `rt_gpu_free` | Device memory allocation/deallocation (`int32_t` elements) |
| `rt_gpu_load_i32` / `rt_gpu_store_i32` | Scalar read/write (plain memory on the simulator, device pointers for CUDA/ROCm) |
| `rt_gpu_copy_from_host_i32` / `rt_gpu_copy_to_host_i32` | Bulk host <-> device transfers |
| `rt_gpu_launch_ptx` / `rt_gpu_launch_hsaco` | Launch kernels emitted by LLVM as PTX or HSA Code Object |
| `rt_gpu_await_event` | Waits for a kernel event to complete (returns 1 on success, 0 on failure) |

## Relationship to Surrounding Files and Pipeline Stages

- **RuntimeABI.h** — Source of the struct definitions used in all function signatures in this file. `Runtime.h` `#include`s it directly.
- **FragmentPluginABI.h** — Provides types such as `LunaFragmentInvocationV1`, referenced by `rt_fragment_plugin_invoke`. `Runtime.h` `#include`s it directly.
- **Runtime.cpp** — Implementation of all declarations in this file. Every `rt_*` function has a corresponding definition in `Runtime.cpp`.
- **ApplicationHostServices.h/.cpp** — Provide `lunaApplicationConsoleV1` / `lunaApplicationFileSystemV1`, used by `rt_install_application_host_services_v1`.
- **Generated IR** — Compiler-generated code calls the `rt_*` functions directly, e.g. `rt_alloc`, `rt_array_index_or_abort`, `rt_dynamic_fragment_*`, `rt_gpu_*`.

## Further Reading

- Full semantics of each struct field in `RuntimeABI.h`
- ABI specification of fragment descriptors and calling conventions in `FragmentPluginABI.h`
- Behavior of each function under the default implementation in `Runtime.cpp`
- Factory functions for console input and file system services in `ApplicationHostServices.h`


---

---
title: RuntimeABI.h
source: src/runtime/RuntimeABI.h
language: en
audience: Luna runtime implementers / ABI designers / embedding hosts
---
