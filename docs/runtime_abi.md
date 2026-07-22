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
