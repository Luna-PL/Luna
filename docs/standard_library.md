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

迭代管道的第一阶段已由编译器对数组、切片和 `range` 直接融合，不要求 `Vec`
成为 builtin。Core 已保留 `Option`、`Iterator`、`IntoIterator` 和
`FromIterator` 的稳定身份；下一阶段会物化这些 trait/adapter 布局，再让标准库
动态容器和用户类型接入。当前边界见[迭代、管道与容器边界](iterators.md)。
