# C FFI 边界

Luna 目前提供显式的 C ABI 边界。导入使用 `extern "C"`；若目标符号名和 Luna 名不同，使用 `as` 指定链接名：

```luna
extern "C" fn puts(message: cstr) -> i32;
extern "C" fn malloc(size: usize) -> linear raw<u8>;
extern "C" fn c_free(linear pointer: raw<u8>) as "free";
```

导出的 C ABI 函数使用 `export "C" fn`。`extern` 声明不可同时 `export`，也不能是 `constexpr` 或泛型。

当前稳定支持的 ABI 类型是整数、浮点、`cstr`、`raw<T>`、`unit`，以及指向这些标量的引用。`string`、结构体、枚举、闭包、trait 对象和 `device_buffer<T>` 不可穿过 C ABI；编译器会在声明处拒绝它们。

每个 C ABI 参数必须写出显式类型。需要将所有权移交给外部函数时，形参应标记为 `linear`，调用点必须写 `move value`；例如 `c_free(move buffer)`。外部 allocator 的拥有型返回值必须写为 `-> linear raw<T>`；其调用结果自动成为 linear，即使调用点只写 `let buffer = malloc(16);` 也必须被移动或消费，不能静默丢弃。这个契约可由普通 Luna 函数以相同的 `-> linear raw<T>` 返回标记继续转发。当前 owning-return 契约仅允许 `raw<T>`，避免把任意 C ABI 值误标为需释放资源。Luna 不会推测外部函数是否保存借用、释放指针或异步访问内存，因此此边界由声明者负责精确表达。

JIT 使用宿主进程符号解析；AOT 使用系统链接器。两条路径均由回归测试覆盖 `puts` 与 `free` 示例。
