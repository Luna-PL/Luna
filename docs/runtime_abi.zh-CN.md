# Runtime ABI v1 与分配域

Luna 0.2.0-alpha 开始把语言基础能力与原始 C FFI 分离。编译器生成的
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

## 四个不可混用的资源域

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
进行验证。v1 只能在结构尾部追加字段，消费方必须先检查 `struct_size`。
`LunaRuntimeModuleContextV1` 预留给经验证的 Moon 容器和未来插件 ABI v2，
使动态模块通过授权的服务表工作，而不是依赖进程里偶然可见的 C
符号。当前 external fragment ABI v1 仍保持原样，不会被无声扩展。

## 按需付费

- 没有 `new`、清理或 `print` 的静态代码不会生成对应 runtime 调用。
- 普通分配调用是一次固定 `rt_alloc` 边界；可替换服务表的间接调用发生在
  runtime 内部，不在每个语言对象内嵌 capability/header。
- 可执行内存、GPU、动态选择和动态 apply 都是独立 capability，不会因为链接
  `libruntime` 就自动启用。

