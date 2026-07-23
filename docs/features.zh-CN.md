# Luna 主要特性概览

[English](features.md) | [简体中文](features.zh-CN.md)

本文是 Luna 0.2.0-alpha 已实现语言表面的导航图。每节只解释该能力在整体架构中的
位置，并链接到具体参考或设计文档。实验性状态和已知限制见
[Alpha 发布说明](alpha_release.md)。

## 经过验证的编译管线

Luna 源码先经过类型、trait 和所有权检查，再降级到 MoonIR。MoonIR 在优化前后
都会验证；LLVM JIT 与 AOT 统一消费 MoonIR，不再分别读取 Luna AST。

MoonIR 未来还将作为可移植 Moon 容器和 MoonRuntime 的语言级安全边界。容器加载、
验证和 hotspot runtime 目前属于已预留但尚未完整交付的接口。

具体设计：[MoonIR Metadata 重构 RFC](Arch/MoonIR_Metadata_Refactor_RFC.md)。

## 类型、泛型与 trait

结构体和枚举默认采用结构化身份，`nominal` 显式创建名义边界。语义类型身份不会
和目标平台物理布局混为一谈。泛型按实际需要实例化，普通静态 trait 在编译期
解析，不需要 vtable 或运行时 trait 查找。
具名 `constraint` 谓词提供独立的 C++ concept 风格边界：用户可以组合编译期
类型命题，编译器在泛型实例化点完成证明，constraint 不进入 MoonIR。

具体设计：[类型与身份](types.md)和
[类型系统身份 RFC](Arch/Type_System_Identity_RFC.md)。

## 所有权与显式成本

Luna 将所有权关系和使用次数分开：值可以是拥有、共享借用或可变借用，同时具有
copy、affine 或 linear 使用约束。`move`、`borrow`、路径敏感清理和线性 kernel
event 会被检查器及 MoonIR 明确记录。

具体设计：[所有权与仿射模型](Arch/Ownership_Affine_Model_RFC.md)。

## Metadata 与动态能力

Metadata schema 是一等声明。静态 `select` 会在真实、可遍历的声明与 metadata
视图上执行普通用户函数，并在编译期完成。对象已经可由名称和签名确定时，可以
直接做静态声明反射，不需要 Metadata 或 Select。只有使用 `runtime` 才保留
运行时可见信息，`dynamic select` / `dynamic apply` 则显式承担运行时绑定成本。
普通编译期 Metadata 不会被悄悄带入运行时。

具体说明：[Metadata 与 selector](versioning.md)。

## Package 与 module

Package ID 使用反向 DNS 名称，是版本和依赖单元；module 使用 `::`，是 package
内部的源码命名空间。manifest、workspace、lockfile、`using ... as` 和显式
`export` 共同定义当前本地 package 边界。

具体说明：[Package 与 module](packages.md)。

## 结构化 fragment

`interceptor`、`context`、`slot`、`resume`、`abort` 和 `apply` 用于表达结构化
控制效果。静态路径直接结构化降级；动态路径必须显式声明，并只保留运行时分派
所需的 descriptor。

具体说明：[Fragment 与 slot](fragments.md)和
[外部 fragment 插件](dynamic_plugins.md)。

## Runtime 与 C FFI

编译器生成的分配、清理和输出经过带版本的 Luna Runtime ABI。用户 C 互操作仍是
显式 `extern "C"` 边界，并受到类型与所有权契约约束。

具体说明：[Runtime ABI](runtime_abi.md)和 [C FFI](ffi.md)。

## 异构计算

可达的 `kernel fn` 可以在始终可用的 CPU 模拟器上运行，也可以为显式请求的
CUDA/ROCm target 生成设备代码。`launch` 返回线性 event，`await` 完成操作，
工作进行期间 device buffer 仍受所有权检查保护。

具体说明：[异构计算](heterogeneous_compute.md)。

## 编译期能力与集合

Alpha 已包含 `const`、`constexpr`、编译期类型反射、安全定长数组、索引和借用
切片。堆拥有通用容器和格式化标准 I/O 仍属于后续路线。

具体说明：[编译期能力](compile_time.md)、[数组与切片](arrays.md)和
[标准库雏形](standard_library.md)。
