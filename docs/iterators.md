# 迭代、管道与容器边界

Luna 当前的目标是“动态扩展 + 有限控制流增强 + 系统语言性能”。迭代机制因此采用
惰性、可融合的普通控制流模型，不引入完整代数效应，也不建立独立的 effect summary。
实现与安全事实继续由编译器推导到只读 `sysmeta`。

## 当前可用表面

数组和切片提供以下入口：

```luna
values.iter()       // Iterator<&T>
values.iter_mut()   // Iterator<&mut T>
values.into_iter()  // Iterator<T>，当前仅支持 Copy 元素
```

整数半开区间使用 `range(start, end)`。惰性适配器包括：

```luna
iterator.map(fn(T) -> U)
iterator.filter(fn(T) -> bool)
iterator.take(count)
```

终结操作包括：

```luna
iterator.fold(initial, fn(Acc, T) -> Acc)
iterator.for_each(fn(T) -> unit)
iterator.count()
```

`for item in iterator { ... }` 也是终结点。直接遍历数组按值产生元素；直接遍历切片按
共享引用产生元素。`range` 是半开区间，所以上界不包含在结果中。

## 实现模型

适配器链在前端形成编译器域的临时 iterator recipe。MoonIR 保留已验证的源、适配器
顺序、元素输入输出类型和借用模式；LLVM lowering 将整条链融合为一个循环。它不会为
`map` 或 `filter` 建立中间数组，不依赖 `Vec`，也不调用 iterator runtime ABI。

recipe 当前不能绑定到局部变量、返回或跨 ABI 逃逸，必须立即由 `for`、`fold`、
`for_each` 或 `count` 消费。这一限制保证初始实现无需尚未完成的用户可见适配器布局、
逐元素 drop 状态和闭包环境布局。

源表达式、适配器参数和终结参数按从左到右的源代码顺序求值。`filter` 跳过元素，
`take` 只计算流经它的元素，因此适配器顺序具有通常的惰性管道含义。

## 所有权

- `iter()` 在消费期间持有共享借用。
- `iter_mut()` 在完整 `for` 循环期间持有独占可变借用。
- `into_iter()` 和直接数组遍历当前只接受 `Copy` 元素；数组值使用栈上快照，不会
  因循环体修改原数组而改变后续迭代结果。
- `map` 的输入与输出、`filter` 的输入以及 `fold` 的累加器当前必须为 `Copy`。
  move-only 支持必须先实现适配器逐元素初始化位与提前退出时的 drop glue。
- 适配器闭包当前必须符合既有的无捕获闭包能力；设备 kernel 中的管道尚未开放。

这些约束是阶段性安全边界，不是最终的 Iterator 抽象能力。

## 容器是否是 builtin

迭代协议不要求动态容器成为 builtin。固定数组和借用切片仍是语言基础类型，因为其
长度、布局、边界检查和借用关系直接参与类型检查与代码生成。未来的 `Vec<T>`、链表、
哈希表及用户容器应位于 Core/标准库，通过稳定的 `IntoIterator`/`Iterator` trait
接入；编译器只保留必要的内建入口和优化识别。

目前已保留稳定核心身份：

- `org.luna.core::prelude::Option`
- `org.luna.core::iter::Iterator`
- `org.luna.core::iter::IntoIterator`
- `org.luna.core::iter::FromIterator`

当前编译器 recipe 不通过运行时反复调用 `next() -> Option<T>`，因为静态已知链可以
直接融合。下一阶段物化 Core adapter struct 和用户可实现 trait 时，`Option<T>` 将
成为通用 `next` 协议的返回类型；优化器仍可识别并消除这些抽象。

## 与效应和 sysmeta 的关系

迭代管道是普通、有限、结构化控制流，不是 continuation handler，也没有恢复或多次
恢复语义，因此不应被建模成代数效应。当前 recipe 的 host-only 能力是编译器推导的
`sysmeta` 事实。后续若开放设备 lowering、可暂停迭代器或协程适配器，编译器应继续
推导 `maySuspend`、存储和 ABI 事实，而不是要求用户维护平行的 effect summary。

## 后续演进顺序

1. 物化 Core `Option<T>`、`Iterator`、`IntoIterator` 和基础 adapter struct。
2. 允许无捕获 recipe 逃逸，并验证泛型单态化后仍可零成本展开。
3. 加入 move-only item 的初始化位、提前退出清理与 `Drop` glue。
4. 增加 `collect`/`FromIterator`；动态容器保持标准库类型。
5. 完成闭包捕获布局后允许捕获式 `map`/`filter`。
6. 最后再与无栈协程对接异步迭代，并由 `sysmeta` 推导暂停和 frame 需求。
