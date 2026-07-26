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

## 错误与 panic

可恢复失败使用普通值 `Result<T, E>`。通用 enum/Result 穷尽 `match` 绑定活动载荷；
后缀 `?` 携带路径敏感清理义务，并在错误类型不同时调用唯一、静态解析的
`From<Source> for Target`。冻结的 inline ADT 布局支持普通 enum、数组、slice 与
嵌套 Result，清理只作用于当前 tag 对应的载荷。

`panic(string)` 是 abort 边界，不展开语言栈。该模型不引入完整代数效应，也没有
独立 effect summary；编译器所需事实继续由只读 sysmeta 推导。

具体设计：[Result 与 panic RFC](Arch/Error_Result_Panic_RFC.md)和
[标准错误与外部转换边界](Arch/Standard_Error_Boundaries_RFC.md)。

## 迭代与管道

数组、slice 和整数 `range` 可组成惰性的 `map`/`filter`/`take`，并终结到
`for`、`fold`、`for_each` 或 `count`；编译器已知链融合为一个 LLVM 循环。
Core 提供具有稳定 package identity 的 `Option`、`Iterator`、`IntoIterator`、
`FromIterator` 与基础 adapter。用户 trait 成员调用静态解析到唯一 impl symbol，
用户 Core Iterator 可直接驱动 `for`。

协议产生的 move-only 元素以 `Some` tag 作为逐轮初始化状态：正常循环体末尾和
函数提前返回路径都恰好清理一次，已经移动走的元素不再清理。`for` 现会插入唯一的
静态 Core `IntoIterator` 转换并持有隐藏状态；consuming move-only 数组使用逐元素
初始化位，`filter`/`take` 会清理拒绝项，`map` 可从 Copy 输入产生 move-only
输出，也可通过显式 owning 参数消费 move-only 输入。lambda 函数体已进入路径敏感
所有权检查，`fold`/`for_each`/`count` 会持有 move-only 终结 recipe 的隐藏状态。
affine move-only fold 累加器现使用独立 replacement 初始化位并把最终值转移给
调用者。linear 累加器与捕获式 closure environment 仍是后续边界。
无捕获 recipe（包括拥有 move-only 数组源的 recipe）现可物化为 affine、单次消费
的局部栈值。owning recipe 使用逐元素初始化位，在消费、丢弃和提前返回路径恰好清理
剩余元素，同时保持静态融合且不引入 iterator runtime allocation。

具体说明：[迭代、管道与容器边界](iterators.md)。

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
