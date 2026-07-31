# 标准库雏形

标准库当前在主仓库的 `stdlib/` workspace 中逻辑独立，等 package、Moon
容器和 Runtime ABI 冻结后再整体迁移到独立仓库。

```text
stdlib/
├── luna.workspace
├── luna.lock
├── core/   # org.luna.core
└── std/    # org.luna.std -> org.luna.core
```

`org.luna.core` 和 `org.luna.std` 是两个独立 package，不是子包。`core` 只容纳
不需要 host/OS 服务的类型和操作；`std` 可通过 Runtime ABI 使用 I/O、分配和
其他宿主 capability。

`org.luna.std` 已预留真实的 `io` module，但当前仍没有把编译器内建
`print` 包装成库函数。在动手前需要先冻结格式串验证、`Format`/`Scan`
trait、静态/动态格式付费边界和错误返回类型。

错误层采用“具体错误 + 静态 `From` 聚合”。Core 已物化无 host 依赖的
`ErrorCode`、`InvalidArgumentError`、`BoundsError`、`UtfError`、
`LayoutError` 和 `AllocError`；它们使用冻结的 inline ADT ABI。依赖易失错误文本
的语言层边界错误仍暂不公开。Runtime 已提供 caller-owned 的
`domain/code/message` 快照，并规定 owned diagnostic 分配失败时保留机器字段、
省略文本；下一步是在 Std 中物化安全 adapter 和具体错误类型，而不是直接暴露
兼容用的 `last_error` 指针。边界设计见
[标准错误与 FFI/Runtime 转换 RFC](Arch/Standard_Error_Boundaries_RFC.md)。

迭代管道的第一阶段已由编译器对数组、切片和 `range` 直接融合，不要求 `Vec`
成为 builtin。Core 已物化 `Option`、`Iterator`、`IntoIterator`、
`FromIterator` 以及 `Map`/`Filter`/`Take` adapter。用户 impl 的静态成员调用、
Core Iterator `for` 协议以及协议 move-only item 的正常/提前返回清理现已完成；
`for` 也已隐式物化唯一的 Core `IntoIterator` 并清理隐藏状态。consuming array
recipe 现使用逐元素初始化位，move-only `filter`/`take` 及 `map` 的 move-only
输入/输出具有拒绝、截断和提前返回清理。lambda 函数体已进入路径敏感所有权检查，
`fold`/`for_each`/`count` 也会物化 move-only 终结 recipe 状态，affine
move-only fold 累加器使用独立 replacement 初始化位。无捕获 adapter 现已支持
affine 局部栈物化且继续静态融合；move-only owning recipe 使用隐藏源快照和逐元素
初始化位，覆盖消费、未消费和提前返回清理。终结 `collect::<Target>()` 通过 Core
`FromIterator` 的静态
`begin/push/finish` builder 协议完成，不物化跨 ABI iterator，也不分配中间容器；
Copy 和 move-only item 的融合、截断与尾部清理均已覆盖 JIT/AOT。待完成的是把内部
Drop 状态冻结为跨函数 Core adapter 布局、closure environment 与泛型 impl
specialization。当前边界见
[迭代、管道与容器边界](iterators.md)。

无栈协程和并发关键字不属于当前标准库前置工作；至少等上述 Core 类型、Drop 和
错误 adapter 稳定后再设计。
