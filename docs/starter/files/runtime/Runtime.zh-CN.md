# src/runtime/Runtime.cpp

Luna 运行时全部 C ABI 入口点的默认实现，涵盖宿主服务安装、内存管理（普通/RC/ARC）、控制台 I/O、GPU 后端（CUDA/ROCm）动态加载、片段插件加载、动态片段分派和错误快照。

## 这个文件做什么

- 实现 `Runtime.h` 中声明的全部 `rt_*` 函数。
- 提供默认的 `LunaHostServicesV1`、`LunaAllocatorV1`、`LunaConsoleV1` 实例（`defaultHostServices`、`defaultAllocator`、`defaultConsole`）。
- 提供应用级宿主服务 `applicationHostServices`，含控制台输入和文件系统。
- 通过 `std::atomic` 实现宿主服务的单次安装与三阶段生命周期（0=可配置，-1=安装中，1=已激活）。
- 管理 GPU 运行时状态（`GpuRuntimeState`）：在命名空间匿名作用域内通过函数静态局部变量持有 CUDA Driver API 与 HIP API 的函数指针，仅在用户选择 `cuda`/ `rocm` 后端时动态加载（`dlopen`/ `LoadLibrary`）。
- 管理片段插件状态（`FragmentPluginState`）：加载的共享库列表、错误码与错误消息。
- 处理动态片段选择：从环境变量 `LUNA_FRAGMENT_<SLOT>` 读取名称。
- 实现 GPU 性能分析（`LUNA_GPU_PROFILE=1`）：进程退出时通过 `std::atexit` 报告累计内核时间。

## 关键结构体·类·枚举

### 匿名命名空间类定义

| 类型 | 用途 | 类比 C++ |
|---|---|---|
| `AtomicSharedCounter` | `std::atomic<uint64_t>` 的别名 | 原子整数 |
| `SharedCounter` | `union` 联合体，可解释为普通 `uint64_t`（RC）或 `AtomicSharedCounter`（ARC） | 类似 `std::variant<uint64_t, std::atomic<uint64_t>>` |
| `SharedAllocationHeader` | RC/ARC 分配头部：分配基址、大小、对齐、计数、`LunaDropCallbackV1` 析构回调 | 类似 `std::shared_ptr` 的控制块 |
| `CudaApi` | CUDA Driver API 函数指针集合（16 个函数指针） | 函数指针表 |
| `HipApi` | HIP API 函数指针集合（15 个函数指针） | 函数指针表 |
| `CudaEventRecord` | CUDA 启动/完成事件对 | — |
| `HipPendingEvent` | HIP 启动/完成事件对 | — |
| `GpuRuntimeState` | GPU 运行时全局状态：初始化标志、后端类型、CUDA/HIP 句柄、模块缓存、函数缓存、事件缓存、性能分析累计时间 | 单例状态对象 |
| `FragmentPluginState::Loaded` | 已加载的片段插件：共享库指针、描述符指针、路径 | — |
| `FragmentPluginState` | 片段插件运行时状态：加载列表、错误码、错误消息 | — |

### 关键常量

| 常量 | 值 | 用途 |
|---|---|---|
| `defaultHostServices` | `LunaHostServicesV1` | 默认宿主服务，仅提供分配器 + 控制台输出 |
| `applicationHostServices` | `LunaHostServicesV1` | 应用级宿主服务，额外提供控制台输入 + 文件系统 |
| `hostServicesPhase` | `std::atomic<int>` | 三阶段：0=可配置，-1=安装中，1=已激活 |

## 关键函数·方法

### 宿主服务安装（`activateHostServices` / `validHostServices` 等）

| 函数 | 用途 |
|---|---|
| `activateHostServices()` | 自旋 CAS 将阶段从 0 推进到 1，返回已安装的 `LunaHostServicesV1*`；类似一次性的 double-checked locking |
| `validHostServices` | 深度校验：magic 字段、abi_version、struct_size、reserved_zero、capabilities 必须包含 `LUNA_HOST_CAP_ALLOCATOR`，再按 capability 位逐个校验子表 |
| `validAllocator` / `validConsoleOutput` / `validConsoleInput` / `validExecutableMemory` / `validFileSystem` | 各子表的有效性校验函数 |

### 默认分配器回调

| 函数 | 用途 |
|---|---|
| `defaultAllocate` | 对齐 `<= alignof(max_align_t)` 时用 `std::malloc`；大对齐在 POSIX 用 `posix_memalign`，Windows 用 `_aligned_malloc` |
| `defaultDeallocate` | 与分配路径严格配对：POSIX 用 `std::free`，Windows 大对齐使用 `_aligned_free` |
| `defaultReallocate` | 普通对齐使用 `std::realloc`；大对齐则分配新内存、memcpy、再释放旧内存 |

### 引用计数内存

| 函数 | 用途 |
|---|---|
| `rt_rc_allocate_v1` | 分配 `SharedAllocationHeader` + 数据区，返回数据区指针，rc 初始化为 1 |
| `rt_rc_retain_v1` | 非原子自增 rc |
| `rt_rc_release_v1` | 非原子自减 rc，归零则调用 drop 回调并释放整个块 |
| `rt_arc_allocate_v1` / `rt_arc_retain_v1` / `rt_arc_release_v1` | 同上，但使用 `std::atomic` 的 `fetch_add` / `fetch_sub`，支持跨线程安全 |

### GPU 后端

| 函数 | 用途 |
|---|---|
| `rt_gpu_initialize` | 读取 `LUNA_GPU_BACKEND`，若为 `cuda` 则 `dlopen("libcuda.so.1")` 并加载 16 个 CUDA Driver API 符号；若为 `rocm` 则加载 `libamdhip64.so` 并加载 15 个 HIP 符号 |
| `rt_gpu_launch_ptx` | 缓存 `cuModuleLoadData`/ `cuModuleGetFunction`，调用 `cuLaunchKernel` |
| `rt_gpu_launch_hsaco` | 同上，但使用 HIP 加载 HSA Code Object |
| `rt_gpu_await_event` | 调用 `cuEventSynchronize`/ `hipEventSynchronize`，记录耗时到 `profiledKernelMs` |
| `checkCuda` / `checkHip` | 每次 API 调用后的错误检查，失败时调用 `setGpuError` |
| `reportGpuProfileAtExit` | 通过 `std::atexit` 注册，打印 `Luna GPU profile: kernel_ms=...` |

### 动态片段分派

| 函数 | 用途 |
|---|---|
| `dynamicFragmentEnvironmentKey` | 将 slot_name 转换为全大写环境变量键名 `LUNA_FRAGMENT_<SLOT>`，非字母数字下划线字符映射为 `_` |
| `rt_dynamic_fragment_select` | 读取该环境变量，若为空则返回 fallback_name |
| `rt_dynamic_fragment_matches` | 字符串比较 selected_name 与 candidate_name |
| `rt_dynamic_fragment_report_unknown_and_abort` | 打印错误消息并调用 `std::abort` |

### 片段插件加载

| 函数 | 用途 |
|---|---|
| `rt_fragment_plugin_load` | 调用 `lunaOpenLibrary`（`dlopen`/ `LoadLibrary`），查找 `luna_fragment_plugin_descriptor_v1` 符号，校验描述符，注册到 `FragmentPluginState` |
| `rt_fragment_plugin_is_registered` | 遍历已加载列表，按 slot_name + fragment_name + contract_hash 三元组匹配 |
| `rt_fragment_plugin_invoke` | 查找匹配插件并调用其 `entry` 函数指针 |

### 工具函数

| 函数 | 用途 |
|---|---|
| `lunaOpenLibrary` / `lunaLoadSymbol` / `lunaCloseLibrary` | 跨平台动态库加载适配器，封装 POSIX `dlopen`/ `dlsym`/ `dlclose` 与 Windows `LoadLibraryA`/ `GetProcAddress`/ `FreeLibrary` |
| `kernelFunctionCacheKey` | 生成模块键 + 空字符分隔 + 内核名的缓存键字符串，防止不同模块中同名内核的句柄冲突 |
| `sharedHeader` | 从 RC/ARC 数据指针反推 `SharedAllocationHeader` 指针 |

## 与周边文件·阶段的关系

- **Runtime.h** —— 本文件实现的全部函数声明。`Runtime.cpp` 直接 `#include` 它。
- **RuntimeABI.h** —— 提供 `LunaHostServicesV1` 等结构体定义和所有 `LUNA_*` 常量宏。`Runtime.cpp` 直接 `#include` 它。
- **ApplicationHostServices.h** —— 提供 `lunaApplicationConsoleV1` / `lunaApplicationFileSystemV1`，用于构造 `applicationHostServices` 常量。
- **FragmentPluginABI.h** —— 提供 `LunaFragmentPluginDescriptorV1` 等类型，用于插件加载验证。
- **生成代码（Generated IR）** —— 编译器生成的代码在运行时调用本文件实现的 `rt_*` 函数。
- 本文件不依赖任何 Luna 编译器内部结构，仅依赖标准 C/C++ 运行时和平台动态加载 API。

## 延伸阅读

- `RuntimeABI.h` 中所有结构体字段的完整语义
- `FragmentPluginABI.h` 中片段插件描述符的 ABI 规范
- `ApplicationHostServices.cpp` 中文件系统与控制台输入的具体实现
- CUDA Driver API 文档（`cuModuleLoadData`/ `cuLaunchKernel` 等）
- ROCm HIP API 文档（`hipModuleLoadData`/ `hipModuleLaunchKernel` 等）


---

---
title: Runtime.h
source: src/runtime/Runtime.h
language: zh-CN
audience: Luna 运行时实现者 / 嵌入宿主
---

# src/runtime/Runtime.h

Luna 运行时的 C 语言 ABI 入口点总声明头文件，定义了宿主环境安装、内存管理、控制台 I/O、GPU 后端管理、片段（Fragment）插件加载与动态分派、以及数组边界检查等全部外部可调用函数。

## 这个文件做什么

- 声明宿主服务（Host Services）的安装与查询接口：`rt_install_host_services_v1`、`rt_install_application_host_services_v1`、`rt_host_services_v1`。
- 声明 Luna 托管内存分配器：普通分配（`rt_alloc`/ `rt_realloc`/ `rt_dealloc`）、引用计数 RC（`rt_rc_allocate_v1`/ `rt_rc_retain_v1`/ `rt_rc_release_v1`）、原子引用计数 ARC（`rt_arc_allocate_v1`/ `rt_arc_retain_v1`/ `rt_arc_release_v1`）。
- 声明控制台输出、输入及格式化打印工具函数（`rt_print_i32`、`rt_print_cstr`）。
- 声明 0.2 版兼容性桥接层（`rt_compat_console_write_cstr_0_2` 等五函数）。
- 声明 GPU 后端初始化、设备内存分配、数据传输、内核启动与事件等待函数簇（`rt_gpu_*`）。
- 声明片段（Fragment）动态分派（`rt_dynamic_fragment_select`/ `rt_dynamic_fragment_matches`/ `rt_dynamic_fragment_report_unknown_and_abort`）与外部插件加载（`rt_fragment_plugin_load`/ `rt_fragment_plugin_invoke` 等）。
- 声明数组越界安全检查 `rt_array_index_or_abort`。
- 错误快照查询 `rt_runtime_error_snapshot_v1`。

函数按功能分组、以独立的块注释分隔，整体被 `extern "C"` 包裹以保证 C 链接约定。

## 关键结构体·类·枚举

本文件不定义结构体，所有结构体定义在 `RuntimeABI.h` 中，本文件仅在其函数签名中引用以下类型：

- `LunaHostServicesV1` —— 宿主服务描述符，包含分配器、控制台、可执行内存、文件系统等子表。
- `LunaRuntimeErrorSnapshotV1` —— 运行时错误快照，含 domain、code、message_size。
- `LunaDropCallbackV1` —— 析构回调函数指针类型（`void(*)(void* value_storage)`）。
- `LunaFragmentInvocationV1` —— 片段调用参数包，定义在 `FragmentPluginABI.h`。

## 关键函数·方法

### 宿主服务安装

| 函数 | 用途 |
|---|---|
| `rt_install_host_services_v1` | 安装自定义宿主服务描述符；必须在首次运行时服务使用前调用，传入 `nullptr` 被拒绝 |
| `rt_install_application_host_services_v1` | 安装进程级控制台输入 + 文件系统服务，供普通生成的 Luna 应用入口使用 |
| `rt_host_services_v1` | 返回当前已安装的 `LunaHostServicesV1*` |

### 内存管理

| 函数 | 类比 C++ |
|---|---|
| `rt_alloc` / `rt_realloc` / `rt_dealloc` | 类似 `::operator new` / `std::realloc` / `::operator delete`，但显式传 alignment |
| `rt_rc_allocate_v1` / `rt_rc_retain_v1` / `rt_rc_release_v1` | 类似 `std::shared_ptr` 的非原子引用计数实现；分配时传入 `LunaDropCallbackV1` 析构回调 |
| `rt_arc_allocate_v1` / `rt_arc_retain_v1` / `rt_arc_release_v1` | 原子引用计数（`std::atomic<int>` 版 `shared_ptr`），可用于跨线程共享 |
| `rt_panic_cstr` | 不可恢复错误，打印消息并终止进程（类似 `std::terminate` + 自定义消息） |

### 控制台 I/O 与兼容桥接

| 函数 | 用途 |
|---|---|
| `rt_print_i32` / `rt_print_cstr` | 语言级整数/字符串打印，使用稳定的 Luna ABI，避免 JIT 对象解析平台 `printf` |
| `rt_compat_console_write_cstr_0_2` 等 | 0.2 版标准库临时适配器，后续会被 0.3 安全 IO 层替换 |
| `rt_array_index_or_abort` | 生成代码在 GEP 前调用；越界则终止，防止未定义行为 |

### 片段（Fragment）动态分派

| 函数 | 用途 |
|---|---|
| `rt_dynamic_fragment_select` | 根据 slot_name 从环境变量 `LUNA_FRAGMENT_<SLOT>` 读取选择 |
| `rt_dynamic_fragment_matches` | 检查 selected_name 是否等于 candidate_name |
| `rt_dynamic_fragment_report_unknown_and_abort` | 选择到未知片段时打印错误并终止 |

### 外部片段插件

| 函数 | 用途 |
|---|---|
| `rt_fragment_plugin_load` | 加载共享库（`dlopen`/ `LoadLibrary`）并校验描述符，进程级常驻 |
| `rt_fragment_plugin_last_error` | 返回最近一次加载错误的字符串 |
| `rt_fragment_plugin_is_registered` | 按 slot_name + fragment_name + contract_hash 三元组查询注册状态 |
| `rt_fragment_plugin_invoke` | 调用已注册的片段入口点 |
| `rt_fragment_plugin_report_error_and_abort` | 报告插件错误并终止 |

### GPU 后端

| 函数 | 用途 |
|---|---|
| `rt_gpu_initialize` | 根据 `LUNA_GPU_BACKEND`（`sim`/ `cuda`/ `rocm`）初始化后端 |
| `rt_gpu_backend_name` / `rt_gpu_backend_is_cuda` / `rt_gpu_backend_is_rocm` | 查询当前后端类型 |
| `rt_gpu_alloc_i32` / `rt_gpu_free` | 设备内存分配/释放（`int32_t` 元素） |
| `rt_gpu_load_i32` / `rt_gpu_store_i32` | 标量读写（模拟器为普通内存，CUDA/ROCm 为设备指针） |
| `rt_gpu_copy_from_host_i32` / `rt_gpu_copy_to_host_i32` | 批量主机 <-> 设备传输 |
| `rt_gpu_launch_ptx` / `rt_gpu_launch_hsaco` | 启动 LLVM 发射的 PTX 或 HSA Code Object 内核 |
| `rt_gpu_await_event` | 等待内核事件完成（返回 1 成功，0 失败） |

## 与周边文件·阶段的关系

- **RuntimeABI.h** —— 本文件所有函数签名中使用的结构体定义来源。`Runtime.h` 直接 `#include` 它。
- **FragmentPluginABI.h** —— 提供 `LunaFragmentInvocationV1` 等类型，被 `rt_fragment_plugin_invoke` 引用。`Runtime.h` 直接 `#include` 它。
- **Runtime.cpp** —— 本文件所有声明的实现。每个 `rt_*` 函数都在 `Runtime.cpp` 中有对应定义。
- **ApplicationHostServices.h/.cpp** —— 提供 `lunaApplicationConsoleV1` / `lunaApplicationFileSystemV1`，被 `rt_install_application_host_services_v1` 使用。
- **生成代码（Generated IR）** —— 编译器生成的代码直接调用 `rt_*` 函数，如 `rt_alloc`、`rt_array_index_or_abort`、`rt_dynamic_fragment_*`、`rt_gpu_*` 等。

## 延伸阅读

- `RuntimeABI.h` 中各结构体字段的完整语义
- `FragmentPluginABI.h` 中片段描述符与调用约定的 ABI 规范
- `Runtime.cpp` 中各函数在默认实现下的行为
- `ApplicationHostServices.h` 中控制台输入与文件系统服务的工厂函数


---

---
title: RuntimeABI.h
source: src/runtime/RuntimeABI.h
language: zh-CN
audience: Luna 运行时实现者 / ABI 设计者 / 嵌入宿主
---
