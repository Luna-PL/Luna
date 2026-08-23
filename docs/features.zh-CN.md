# Luna 主要特性概览

[English](features.md) | [简体中文](features.zh-CN.md)

本文是 Luna 0.3 开发期已实现语言表面的导航图。每节只解释该能力在整体架构中的
位置，并链接到具体参考或设计文档。实验性状态和已知限制见
[0.3 总体设计](luna_0.3_design.zh-CN.md)；
[0.2.1 发布说明](alpha_release.zh-CN.md)是历史基线。

## 经过验证的编译管线

Luna 源码先经过类型、trait 和所有权检查，再降级到 MoonIR。MoonIR 在优化前后
都会验证；LLVM JIT 与 AOT 统一消费 MoonIR，不再分别读取 Luna AST。

MoonIR 也是 0.3 host-specific Moon Container 的语言级安全边界。serializer 与
原子验证 loader 已实现，`luna build <package> -t moon` 会生成并自校验 `.moon`；
encoder 会把含泛型的编译器输入投影为 concrete type 和已单态化函数，未解决
recipe 不跨越容器信任边界。随后以 entry/export/host interface/runtime retention
为根计算可达闭包，删除未使用的 concrete code 和 model row，同时保留 direct/dynamic
callee、Drop glue、metadata、fragment 及其 type dependency。
evolution runtime 尚未完成，portable cross-target container 明确延后到 0.3 之后。

具体设计：[架构决策 D001/D002](decisions.md)。

## 类型、泛型与 trait

具名结构体和枚举默认采用名义身份；匿名 `{ field: Type }` record 保持结构化。
`type_same_shape` 检查 shape 但不擦除声明身份，`Target { field: value }` 用于显式具名构造。
泛型按实际需要实例化，普通静态 trait 在编译期
解析，不需要 vtable 或运行时 trait 查找。
具名 `constraint` 谓词提供 C++ concept 风格边界：受约束参数、具名
`where Constraint<T>` 和 inline `where` 谓词都组合编译期类型命题，并在泛型实例化点完成证明，不进入 MoonIR。

规范参考：[完整类型系统](reference/type_system.md)和
[内置类型清单](reference/builtin_types.md)。设计理由见
[0.3 C008/TY002 设计](luna_0.3_design.zh-CN.md#c008具名类型默认名义confirmed)与历史架构决策 D004。

## 所有权与显式成本

Luna 将所有权关系和使用次数分开：值可以是拥有、共享借用或可变借用，同时具有
copy、affine 或 linear 使用约束。`move`、`borrow`、路径敏感清理和线性 kernel
event 会被检查器及 MoonIR 明确记录。
`affine {}` / `linear {}` 为新 binding 设置零成本词法 default；显式
`copy let` / `affine let` / `linear let` contract 可覆盖 default，但不能弱化固有
Resource 要求。只有最终 binding contract 进入 MoonIR。

具体设计：[架构决策 D004](decisions.md)与
[0.3 C009/C010](luna_0.3_design.zh-CN.md#c010linear--affineconfirmed)。

## 错误与 panic

可恢复失败使用普通值 `Result<T, E>`。通用 enum/Result 穷尽 `match` 绑定活动载荷；
后缀 `?` 携带路径敏感清理义务，并在错误类型不同时调用唯一、静态解析的
`From<Source> for Target`。0.2 编译器/MoonIR 的 inline ADT v1 布局支持普通
enum、数组、slice 与嵌套 Result，清理只作用于当前 tag 对应的载荷；该内部布局
不是 C FFI 承诺。

`panic(string)` 是 abort 边界，不展开语言栈。该模型不引入完整代数效应，也没有
独立 effect summary；编译器所需事实继续由只读 sysmeta 推导。

规范参考：[错误模型契约](reference/error_model.md)。设计理由和后续边界见
[架构决策 D005/D006](decisions.md)。

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
当前 structured LLVM path 对 move-only affine fold accumulator 使用初始化位；已完成的
canonical-CFG replacement 则用一个 synthetic local 静态证明 move、重新初始化和最终
transfer，不需要 runtime flag，但该 CFG 尚未成为唯一 backend body。linear accumulator
仍是后续边界。`C016` closure environment 已支持 Copy 捕获与显式 Affine/Linear move
捕获；借用捕获继续拒绝。
无捕获 recipe（包括拥有 move-only 数组源的 recipe）现可物化为 affine、单次消费
的局部栈值。owning recipe 使用逐元素初始化位，在消费、丢弃和提前返回路径恰好清理
剩余元素，同时保持静态融合且不引入 iterator runtime allocation。

具体说明：[迭代、管道与容器边界](iterators.md)。

## Metadata 与 runtime 能力

Metadata schema 是一等声明。静态 `select` 会在真实、可遍历的声明与 metadata
视图上执行普通用户函数，并在编译期完成。对象已经可由名称和签名确定时，可以
直接做静态声明反射，不需要 Metadata 或 Select。只有使用 `runtime` 才保留运行时
可见信息，普通编译期 Metadata 不会被悄悄带入运行时。0.3 phase 模型只有
compile-time 与 runtime；开发编译器里仍存在的旧 `dynamic select` / `dynamic apply`
是等待已冻结 runtime query/Slot replacement 的 0.2 过渡实现，不是第三个 phase 或兼容承诺。

具体说明：[Metadata 与 selector](versioning.md)。

## Package 与 module

Package ID 使用反向 DNS 名称，是版本和依赖单元；module 使用 `::`，是 package
内部的源码命名空间。manifest、workspace、lockfile、`using ... as` 和显式
`export` 共同定义当前本地 package 边界。

具体说明：[Package 与 module](packages.md)。

## 结构化 fragment

`interceptor`、`context`、`slot`、`resume`、`abort` 和 `apply` 当前用于表达结构化
控制流与扩展性。现有 function-local 源码形式是正在迁移的实现，不是最终 0.3 表面。
已确认的目标使用 module-level 二等 Slot identity、名义 Fragment target 和经过验证的
MoonIR composition；确切声明/control 拼写仍是 `TBD-SF006`。

具体说明：[Fragment、slot 与插件](fragments.md)。

## Runtime 与 C FFI

编译器生成的分配、清理和输出经过带版本的 Luna Runtime ABI。用户 C 互操作仍是
显式 `extern "C"` 边界，并受到类型与所有权契约约束。

具体说明：[Runtime ABI 与 C FFI](runtime_abi.md)。

## 异构计算

可达的 `kernel fn` 可以在始终可用的 CPU 模拟器上运行，也可以为显式请求的
CUDA/ROCm target 生成设备代码。`launch` 返回线性 event，`await` 完成操作，
工作进行期间 device buffer 仍受所有权检查保护。

具体说明：[异构计算](heterogeneous_compute.md)。

## 编译期能力与集合

开发编译器已包含 `const`、`constexpr`、编译期类型反射、安全定长数组、索引和借用
切片。堆拥有通用容器和最终格式化标准 I/O 仍属于后续路线；临时、类型明确的
`std::io` console 表面已经可用。

具体说明：[编译期能力](compile_time.md)、[数组、切片与迭代](iterators.md)和
[标准库雏形](standard_library.md)。
