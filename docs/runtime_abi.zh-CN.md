# Runtime ABI v1 与分配域

Luna 0.2.1 开始把语言基础能力与原始 C FFI 分离。编译器生成的
`new`、路径敏感自动清理、显式 `free` 与语言 `print` 只调用 Luna
Runtime ABI，不再直接解析 `malloc/free/printf`。公开的 C 兼容头文件为
`runtime/RuntimeABI.h`。

## 设计边界

Runtime ABI 是分配合约，不是一套固定的分配算法。默认实现可以使用
平台 C/C++ runtime；嵌入式宿主也可在第一次 runtime 服务调用前通过
`rt_install_host_services_v1` 安装自己的 allocator 和 console。安装后的描述符及
其嵌套服务表必须保持到进程结束。

新生成的分配调用为：

```c
void* rt_alloc(size_t size, size_t alignment);
void* rt_realloc(void* pointer, size_t old_size, size_t new_size,
                 size_t alignment);
void  rt_dealloc(void* pointer, size_t size, size_t alignment);
```

拥有型标准库容器改用可恢复的配套边界：

```c
int rt_checked_array_layout_v1(size_t element_size, size_t element_count,
                               size_t alignment, size_t* byte_size,
                               LunaAllocErrorV1* error);
int rt_try_alloc_v1(size_t size, size_t alignment, void** allocation,
                    LunaAllocErrorV1* error);
int rt_try_realloc_v1(void* pointer, size_t old_size, size_t new_size,
                      size_t alignment, void** replacement,
                      LunaAllocErrorV1* error);
```

这些入口通过 caller-owned、无需分配的 `LunaAllocErrorV1` 报告 alignment 非法、数组
size 溢出和 OOM。零 size 分配成功返回 null，且不调用 host allocator。正 size realloc
具有事务性：分配失败时原 allocation 仍有效，并写回 `replacement`。`new_size == 0` 会被
拒绝；调用者必须先析构已初始化元素，再显式调用 `rt_dealloc`。`org.luna.sys::alloc`
已经提供原始 Luna FFI bridge；将该记录映射为 `core::AllocError` 属于后续安全 Alloc adapter。

编译器在分配和每条清理路径上携带同一精确布局，因此自定义 allocator
不需要为每个对象添加隐藏头。`rt_malloc/rt_free` 仅为已经生成的 Alpha IR
保留兼容；新 IR 不使用它们。

不可恢复错误调用 `rt_panic_cstr`。该入口通过已安装 console 的 stderr 写入
诊断并 flush，随后 abort；它不执行语言栈展开或局部 Drop。可恢复错误应使用
`Result<T, E>`，由生成代码在提前返回前执行路径敏感清理。

## 可恢复错误快照

Runtime ABI v1 通过 `rt_runtime_error_snapshot_v1` 暴露可恢复边界错误。调用者
指定 `LUNA_RUNTIME_ERROR_DOMAIN_FRAGMENT_PLUGIN` 或
`LUNA_RUNTIME_ERROR_DOMAIN_GPU`，得到稳定的 `domain/code` 和可选 UTF-8
诊断文本：

```c
LunaRuntimeErrorSnapshotV1 snapshot;
int status = rt_runtime_error_snapshot_v1(
    LUNA_RUNTIME_ERROR_DOMAIN_GPU, &snapshot, NULL, 0);
```

空缓冲区用于查询 `message_size`，有诊断文本时返回
`LUNA_RUNTIME_STATUS_BUFFER_TOO_SMALL`。再次调用时由 adapter 提供
`message_size + 1` 字节即可复制含结尾 NUL 的文本。缓冲不足时也会写入完整
`domain/code/message_size`，并在非空缓冲区中留下安全截断且以 NUL 结尾的文本。

错误身份只由 `domain/code` 决定，文本仅用于诊断。快照操作本身不分配内存；
安全 adapter 若无法为 owned message 分配空间，必须保留机器错误字段并省略文本，
不能 panic，也不能把 `rt_gpu_last_error` 或
`rt_fragment_plugin_last_error` 返回的易失指针存进长期值。两个旧
`last_error` 入口为 Alpha 兼容保留，新 adapter 应使用快照接口，并在失败调用后
立即复制。

## Console input 与 filesystem service

Runtime ABI v1 只在原 output-only console 与 host-services 结构尾部扩展字段。
`LUNA_CONSOLE_V1_OUTPUT_SIZE` 和 `LUNA_HOST_SERVICES_V1_BASE_SIZE` 表示已经发布的旧
前缀。只声明旧 capability 的 host 可继续传入这些前缀 size；声明
`LUNA_HOST_CAP_CONSOLE_INPUT` 或 `LUNA_HOST_CAP_FILESYSTEM` 的 host 必须提供完整扩展表。
默认 runtime 当前不声明这两个新 capability。
编译器生成的 application `main` 会在其他 Runtime 操作前显式调用
`rt_install_application_host_services_v1`。该 application profile 为普通 JIT/AOT executable
增加 native stdin 与 filesystem service；它不会替换 embedding host 已安装的 service table，
没有 application entry 的 library/module 也不会隐式取得这些 capability。

Console input 与 output 共用 `LunaConsoleV1` 表。`read` 可以成功返回少于请求长度的数据；
成功且 `bytes_read == 0` 表示 EOF。可恢复操作失败返回 `LUNA_RUNTIME_STATUS_IO_ERROR` 并填写
caller-owned `LunaIoErrorV1`，不会分配内存或返回进程全局诊断指针。

`LunaFileSystemV1` 提供同步 open/read/write/seek/flush/sync/close/metadata 和基本 path
操作，其合约为：

- path 是有效 UTF-8 的 pointer-plus-length view，不是 NUL-terminated C string；
- adapter 若调用要求 C string 的平台 API，必须把内嵌 NUL 映射为 `INVALID_INPUT`；
- handle 是 opaque unsigned value，零永远无效；
- 成功的 read/write 可以是部分操作，成功读取零字节表示 EOF；
- 可恢复 host 失败返回 `LUNA_RUNTIME_STATUS_IO_ERROR` 并填写 `LunaIoErrorV1`；错误身份为
  `kind/operation/raw_code`，不要求分配文本；其他 Runtime status 表示 ABI/caller contract 失败；
- 除错误记录外的 out parameter 只在 status 成功后使用；
- host 拥有 handle 实现细节，safe Std adapter 拥有 close policy。

native application profile 在 path 转换前验证 UTF-8 与内嵌 NUL，使用 opaque registry ID
而不是暴露 native descriptor；第一次 close 尝试即消费 handle，即使平台 close 报错也不允许
再次使用。`flush` 只清除 library buffering，而 descriptor-based application profile 没有这层
buffer；`sync` 才是显式 durability 操作。

Sys binding 调用固定的 `rt_console_*_v1`、`rt_file_*_v1` 和
`rt_path_metadata_v1` 转发入口，不自行读取或解引用 host table。Runtime 在每次转发时检查已安装
capability；缺失能力映射为不分配的 `LUNA_IO_ERROR_UNSUPPORTED`，已授权调用则保留 host 的
opaque context 与 handle domain。

handle metadata 与 path metadata 是独立操作；safe `File::metadata` 不依赖保留打开文件时的
path。raw ABI 不承诺 `read_exact`、`write_all`、text decoding、递归建目录或 best-effort
Drop。这些 policy 属于 Std，由其重复 partial operation 并显式处理 `INTERRUPTED`。

非 v1 的 `rt_compat_console_*_0_2` helper 是专为 0.2.1 `std::io` 提供的临时 adapter。
它们在不冻结未来 owned String 与 formatting trait 的前提下提供 cstr/i32 formatting 和
有界 line input。这些符号声明于 `Runtime.h`，不属于稳定的公开 `RuntimeABI.h`，
显式 0.3 mode 落地后可被替换。

## 五个不可混用的资源域

1. **Luna host heap**：使用 `rt_alloc/rt_dealloc`，布局由 MoonIR/编译器确定。
2. **Foreign/C resources**：由创建它的库提供释放 capability。
   `LunaOwnedForeignMemoryV1` 是为后续类型化 C FFI adapter 预留的携带者；
   这类指针不能传给 `rt_dealloc`。
3. **Device resources**：使用 `rt_gpu_*` 及后端自己的地址空间/释放协议。CPU
   simulator 只是同一设备 ABI 的 host-backed 实现，不会把设备指针变成 Luna
   host-heap 指针。
4. **Executable memory**：`LunaExecutableMemoryV1` 预留 `reserve -> write -> seal ->
   execute -> release` 的 W^X 合约。默认 runtime 不声明该 capability；未来
   MoonRuntime/hotspot JIT 必须显式获得宿主授权。
5. **Host service handle**：filesystem 以及后续 process/clock resource 是由同一已安装
   host service 创建和释放的 opaque value。它们既不是 Luna pointer，也不是用户代码可通过
   无关 API 关闭的 raw C resource。

## C FFI 边界

Runtime ABI 服务编译器生成的语言操作；用户声明的原始 C 接口使用显式
`extern "C"`，二者不能混用：

```luna
extern "C" fn puts(message: cstr) -> i32;
extern "C" fn malloc(size: usize) -> linear raw<u8>;
extern "C" fn c_free(linear pointer: raw<u8>) as "free";
```

导出使用 `export "C" fn`。`extern` 声明不可同时 `export`，也不能是泛型或
`constexpr`。当前 C ABI 只接受整数、浮点、`cstr`、`raw<T>`、`unit` 及指向这些
标量的引用；`string`、ADT、闭包、trait object、`device_buffer<T>` 和
`Result<T, E>` 都不能直接穿过边界。

每个参数必须有显式类型。所有权移交使用 `linear` 参数和 `move` 调用；外部
allocator 的拥有返回写作 `linear raw<T>`。外来指针必须交还创建它的分配域，
不能传给 Luna `free`/`rt_dealloc`。当前配对责任由声明者承担，后续安全 adapter
可使用 `LunaOwnedForeignMemoryV1` 的 release capability。

可恢复外部 API 应保留原始 status/out-parameter 声明，在普通 Luna adapter 中立即
捕获 errno/status 和诊断快照，再返回 `Result<T, FfiError>`。编译器不会隐式读取
errno，也不会延长外部 `last_error` 指针的生命周期。JIT 通过宿主进程解析用户 C
符号，AOT 通过系统链接器；两条路径均有 `puts`/`free` 回归。

## 版本化与未来模块

`LunaHostServicesV1` 使用 `magic`、`abi_version`、`struct_size` 和 capability bits
进行验证。v1 只能在结构尾部追加字段；消费方必须检查每个已声明 capability 所需的最小
前缀，而不是强制要求最新已知结构 size。嵌套表遵循同一规则。
`LunaRuntimeModuleContextV1` 预留给经验证的 Moon 容器和未来插件 ABI v2，
使动态模块通过授权的服务表工作，而不是依赖进程里偶然可见的 C
符号。当前 external fragment ABI v1 仍保持原样，不会被无声扩展。

## 按需付费

- 没有 `new`、清理或 `print` 的静态代码不会生成对应 runtime 调用。
- 普通分配调用是一次固定 `rt_alloc` 边界。默认 allocator 走 runtime 内的直接
  fast path；宿主安装的 allocator 仍通过可替换服务表调用。两条路径都不会在每个
  语言对象中内嵌 capability 或隐藏分配 header。
- 可恢复容器使用 `rt_try_alloc_v1`/`rt_try_realloc_v1`；失败不会 abort，也不会消费
  既有的正 size allocation。
- 可执行内存、GPU、动态选择和动态 apply 都是独立 capability，不会因为链接
  `libruntime` 就自动启用。
