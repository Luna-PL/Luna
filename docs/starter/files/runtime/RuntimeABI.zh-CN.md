# src/runtime/RuntimeABI.h

Luna 运行时 C 语言 ABI 的核心契约头文件，定义了宿主服务描述符、分配器、控制台、文件系统、可执行内存、错误快照、外来内存等全部结构体类型，以及所有状态码枚举和稳定的 `rt_*` 转发函数声明。

## 这个文件做什么

- 定义 Luna 运行时 ABI 版本常量 `LUNA_RUNTIME_ABI_V1`（值为 1）和宿主服务魔数 `LUNA_HOST_SERVICES_MAGIC_V1`（`"LHS1"`，即 0x4c485331）。
- 定义宿主能力枚举 `LunaHostCapabilityV1`：分配器、控制台输出、可执行内存、控制台输入、文件系统五个能力位。
- 定义所有服务表结构体：`LunaAllocatorV1`、`LunaConsoleV1`、`LunaFileSystemV1`、`LunaExecutableMemoryV1`、`LunaHostServicesV1`。
- 定义错误相关结构体与枚举：`LunaRuntimeStatusV1`、`LunaRuntimeErrorDomainV1`、`LunaRuntimeErrorCodeV1`、`LunaRuntimeErrorSnapshotV1`、`LunaAllocErrorV1`、`LunaIoErrorV1`。
- 定义文件 I/O 相关枚举与结构体：`LunaFileOpenFlagV1`、`LunaSeekWhenceV1`、`LunaFileTypeV1`、`LunaFileMetadataV1`、`LunaFileHandleV1`。
- 定义外来内存所有权包装 `LunaOwnedForeignMemoryV1` 和模块上下文 `LunaRuntimeModuleContextV1`。
- 声明所有 `rt_*` 转发函数，供生成代码直接调用。

## 关键结构体·类·枚举

### 宿主服务聚合

```c
typedef struct LunaHostServicesV1 {
    uint32_t magic;              // LUNA_HOST_SERVICES_MAGIC_V1
    uint32_t abi_version;        // LUNA_RUNTIME_ABI_V1
    uint32_t struct_size;        // >= LUNA_HOST_SERVICES_V1_BASE_SIZE
    uint32_t reserved_zero;      // 必须为 0
    uint64_t capabilities;       // 能力位掩码
    const LunaAllocatorV1* allocator;
    const LunaConsoleV1* console;
    const LunaExecutableMemoryV1* executable_memory;  // 可空
    const LunaFileSystemV1* filesystem;                // 可空（v1 追加字段）
} LunaHostServicesV1;
```

这在整个 Luna 运行时中相当于 C++ 里的"依赖注入容器"——宿主通过它传递分配器、控制台、文件系统等基础设施的实现。

### 子服务表

| 结构体 | 功能 | 关键函数指针 |
|---|---|---|
| `LunaAllocatorV1` | 内存分配器接口 | `allocate` / `reallocate` / `deallocate` |
| `LunaConsoleV1` | 控制台 I/O 接口 | `write` / `flush` / `read`（可选追加） |
| `LunaFileSystemV1` | 文件系统接口（11 个函数指针） | `open` / `read` / `write` / `seek` / `flush` / `sync` / `close` / `metadata` / `path_metadata` / `remove_file` / `create_directory` |
| `LunaExecutableMemoryV1` | 可执行内存 W^X 接口（JIT 用） | `reserve` / `seal` / `release` |

### 错误枚举

| 枚举 | 值域 | 用途 |
|---|---|---|
| `LunaRuntimeStatusV1` | 0, -1 ~ -7 | 函数返回值：OK、INVALID_ARGUMENT、UNSUPPORTED_ABI、ALREADY_ACTIVE、UNSUPPORTED_OPERATION、BUFFER_TOO_SMALL、IO_ERROR、ALLOCATION_ERROR |
| `LunaRuntimeErrorDomainV1` | 0, 1, 2 | 错误域：NONE、FRAGMENT_PLUGIN、GPU |
| `LunaRuntimeErrorCodeV1` | 0 ~ 12 | 错误代码（NONE、UNKNOWN、INVALID_ARGUMENT 等） |
| `LunaAllocErrorKindV1` | 0 ~ 3 | 分配错误类型：NONE、INVALID_ALIGNMENT、SIZE_OVERFLOW、OUT_OF_MEMORY |
| `LunaIoErrorKindV1` | 0 ~ 9 | I/O 错误类型：NONE、NOT_FOUND、PERMISSION_DENIED 等 |
| `LunaIoOperationV1` | 0 ~ 10 | 对哪个 I/O 操作发生的错误（OPEN、READ、WRITE 等） |

### 文件 I/O 相关

| 类型 | 用途 |
|---|---|
| `LunaFileHandleV1` | `uint64_t`，不透明文件句柄，`LUNA_INVALID_FILE_HANDLE_V1`（0）表示无效 |
| `LunaFileOpenFlagV1` | 打开标志位枚举：READ、WRITE、APPEND、TRUNCATE、CREATE、CREATE_NEW |
| `LunaSeekWhenceV1` | 定位方式：FROM_START、FROM_CURRENT、FROM_END |
| `LunaFileTypeV1` | 文件类型：UNKNOWN、REGULAR、DIRECTORY、SYMLINK、OTHER |
| `LunaFileMetadataV1` | 文件元数据：file_type、byte_size |

### 其他包装类型

| 结构体 | 用途 | C++ 类比 |
|---|---|---|
| `LunaOwnedForeignMemoryV1` | 托管的非 Luna 堆内存，含释放回调 | 类似 `std::unique_ptr<void, CustomDeleter>` |
| `LunaRuntimeModuleContextV1` | 动态加载 Moon 模块的上下文 | 依赖注入的运行时上下文 |
| `LunaForeignReleaseFnV1` | 外来资源释放回调函数指针 | 类似 `std::function<void(void*, void*, size_t, size_t)>` |

## 关键函数·方法

### 宿主服务安装

| 函数 | 用途 |
|---|---|
| `rt_install_host_services_v1` | 安装自定义宿主服务描述符，必须为首个运行时调用 |
| `rt_install_application_host_services_v1` | 安装应用级服务（控制台输入 + 文件系统） |
| `rt_host_services_v1` | 获取当前已安装的宿主服务指针 |

### 控制台与文件系统转发

| 函数 | 用途 |
|---|---|
| `rt_console_write_v1` / `rt_console_flush_v1` / `rt_console_read_v1` | 控制台 IO 转发，缺失能力时返回 `LUNA_IO_ERROR_UNSUPPORTED` |
| `rt_file_open_v1` / `rt_file_read_v1` / `rt_file_write_v1` / `rt_file_seek_v1` / `rt_file_flush_v1` / `rt_file_sync_v1` / `rt_file_close_v1` | 文件 I/O 转发 |
| `rt_file_metadata_v1` / `rt_path_metadata_v1` | 文件元数据查询 |
| `rt_remove_file_v1` / `rt_create_directory_v1` | 文件/目录操作 |

### 分配器

| 函数 | 用途 |
|---|---|
| `rt_checked_array_layout_v1` | 计算数组布局（元素大小 × 元素个数 + 对齐），溢出时返回 `ALLOCATION_ERROR` |
| `rt_try_alloc_v1` | 可失败分配，零大小返回 null 而不调用宿主分配器 |
| `rt_try_realloc_v1` | 事务性重分配：失败时保持原指针不变，`new_size == 0` 返回 `INVALID_ARGUMENT` |
| `rt_runtime_error_snapshot_v1` | 拷贝指定 domain 的最新错误快照 |

### 引用计数分配

| 函数 | 用途 |
|---|---|
| `rt_rc_allocate_v1` / `rt_rc_retain_v1` / `rt_rc_release_v1` | 非原子引用计数，单线程使用 |
| `rt_arc_allocate_v1` / `rt_arc_retain_v1` / `rt_arc_release_v1` | 原子引用计数，跨线程使用 |
| `rt_panic_cstr` | 不可恢复错误终止 |

## 与周边文件·阶段的关系

- **Runtime.h** —— 声明与 `RuntimeABI.h` 中结构体类型相同的函数签名，但 `RuntimeABI.h` 额外声明了所有 `rt_*` 转发函数，而 `Runtime.h` 声明的是内部实现函数。
- **Runtime.cpp** —— 使用 `RuntimeABI.h` 中定义的类型和常量，实现所有 `rt_*` 函数。
- **ApplicationHostServices.h** —— 返回 `LunaConsoleV1*` 和 `LunaFileSystemV1*`，其类型定义来自本文件。
- 本文件是所有其他运行时文件的基础依赖，任何实现运行时行为的文件都必须包含它。

## 延伸阅读

- `Runtime.h` 中 C ABI 入口点的完整声明
- `Runtime.cpp` 中本文件各类型的默认实现
- `ApplicationHostServices.cpp` 中文件系统与控制台的具体平台适配


---
