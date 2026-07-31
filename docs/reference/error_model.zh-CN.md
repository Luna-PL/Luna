# Luna 0.2 Alpha 错误模型契约

> 文档类别：语言契约与状态参考
> 适用版本：Luna 0.2.0-alpha
> 状态：核心语义 Frozen for Alpha；adapter/API 部分 Implemented Experimental 或 Planned
> 规范性：核心语义规范；状态表中的内部布局和计划能力非规范
> 首次实现核对：`d0ab31c`（2026-07-31）

本文给出 0.2 Alpha 可以依赖的错误语义，并把尚未完成的标准库/外部 adapter 与
核心模型分开。设计理由见[架构决策 D005/D006](../decisions.md)。

## 1. 失败分类

Luna 只在本基线中定义两类失败：

- 可恢复失败：普通值 `Result<T, E>`；
- 不可恢复进程失败：`panic(message)`。

错误处理不引入异常展开、隐式 handler、effect row、运行时错误类型注册表或全局
`Error` 基类。fragment/slot 是独立的结构化控制模型；错误不会隐式穿过该边界。

## 2. `Result<T, E>`

`Result<T, E>` 是 Value 域、结构身份的编译器内置 tagged union：

```luna
Ok(value)
Err(error)
Ok::<T, E>(value)
Err::<T, E>(error)
```

当前还提供 `is_ok`、`is_err`、`unwrap`、`unwrap_err` 和穷尽 `match`。这些名称是
0.2 Alpha 表面，不代表所有 helper 永久保留为编译器 builtin。

语言契约是：

- 结果只拥有当前 tag 对应的载荷；
- usage 是 `T` 和 `E` usage 的上确界；
- Copy Result 可以普通匹配；
- move-only Result 必须使用 `match move` 才能转移载荷；
- 清理必须先读取 tag，并且只清理活动载荷一次；
- `unwrap`/`unwrap_err` 的 variant 不匹配属于 panic，不是常规错误恢复。

当前 inline ADT v1 布局是编译器/MoonIR Alpha ABI，不是 C FFI 或永久跨版本 ABI。

## 3. `?`

`value?` 必须满足：

1. `value` 为 `Result<T, E>`；
2. 当前普通函数返回 `Result<U, F>`；
3. `E == F`，或存在唯一的直接 `impl From<E> for F`。

成功路径产生 `T`。错误路径必须：

1. 取得活动的 `E`；
2. 执行当前作用域与显式 `return` 相同的清理；
3. 必要时调用已静态选择的 `From::from`；
4. 按外层 `Result<U, F>` 重新构造 `Err`；
5. 返回当前函数。

不得因为两个 Result 看起来具有相同位表示而复用内层容器。不得在运行时搜索转换，
也不得自动搜索多跳转换链。

`?` 当前不得用于 fragment。fragment 的 `return`/`abort` 与宿主函数的返回和清理
所有者不同，必须显式匹配 Result 后选择 fragment 控制行为。

## 4. `From<Source>`

冻结原则是“转换静态、唯一、可审计”。0.2 当前实现进一步限制为：

- Source/Target 都是具体类型；
- 只有一个非泛型 `from(Source) -> Target`；
- 只做一跳精确 TypeId 查找；
- move-only Source 必须通过拥有参数接收；
- coherence/orphan 要求 impl package 拥有 trait、Source 或 Target 中相应的合法
  身份边界。

泛型 `From` 和额外显式调用语法仍是实验性后续工作。它们可以扩展表面，但不得把
转换改成隐式运行时分派。

## 5. `panic` 与 `never`

0.2 Alpha 的 `panic(message)`：

- 接受 `string` 或 `cstr`；
- 向 Runtime stderr console 写入诊断并 flush；
- 调用进程 abort；
- 产生 `never`，LLVM 终点为 `unreachable`；
- 不展开语言栈，也不保证运行局部 Drop。

需要可预测资源释放的失败必须使用 `Result`。未来可以在新的 task/runtime 边界增加
隔离策略，但不得把当前进程 abort 静默改成异常展开或可恢复控制转移。

## 6. 标准错误分层

Core/Std 不定义吞掉所有信息的单一错误枚举：

- Core：参数、边界、UTF、布局、分配等 host-independent 值错误；
- Std：I/O、路径、网络等 host 错误；
- 边界：FFI、Runtime、GPU 和插件错误。

库 API 应返回最窄的具体错误。应用可定义自己的聚合 enum，并用精确静态 `From`
接入 `?`。

`org.luna.core` 当前已物化：

- `ErrorCode`
- `InvalidArgumentError`
- `BoundsError`
- `UtfError`
- `LayoutError`
- `AllocError`

它们是标准库声明的名义 enum，不是新的 `TypeKind`。variant 顺序在 0.2 inline ADT
基线中固定；新增 variant 必须追加并记录兼容性影响。

## 7. C/Runtime 边界

Luna `Result`、enum、`string`、`rc` 和 `arc` 不得直接作为当前 C ABI 类型。安全
adapter 应：

1. 调用 raw C/Runtime API；
2. 在状态被覆盖前捕获 status/errno；
3. 保存稳定 domain/code；
4. 必要时复制诊断文本；
5. 返回普通 Luna `Result<T, BoundaryError>`。

Runtime ABI v1 已提供 caller-owned `domain/code/message` 快照。机器控制必须使用
domain/code；message 只是可选诊断，分配失败时可以省略。旧 `last_error` 借用指针
只用于兼容，不得存入长期错误值。

## 8. 状态矩阵

| 能力 | 状态 | 承诺 |
|---|---|---|
| `Result<T, E>` 语义 | Frozen for Alpha | 普通值、活动载荷、usage/cleanup 规则 |
| `Ok`/`Err`/match | Frozen for Alpha | 当前构造和穷尽匹配行为 |
| `?` 清理后传播 | Frozen for Alpha | 静态直接转换，不跨 fragment |
| `panic` 进程 abort | Frozen for Alpha | 0.2 内不展开栈 |
| `never` bottom type | Frozen for Alpha | 发散表达式可满足普通值位置 |
| 具体 `From` | Implemented Experimental | 一跳、非泛型、唯一静态 impl |
| Result inline ADT v1 | Internal Alpha ABI | 编译器/MoonIR 内部，不是 C ABI |
| Core 值错误 | Implemented Experimental | 名义 enum，variant 顺序受 0.2 约束 |
| Runtime 快照 ABI v1 | Frozen ABI v1 | C-compatible status/domain/code/message snapshot |
| Luna Runtime/FFI adapter | Planned | 尚未作为完整语言层 API 交付 |
| 错误 source 链 | Planned | 不要求全局装箱或动态基类 |
| task-local panic/capture | Planned | 不属于 0.2 语义 |

## 9. 回归证据

当前模型由以下回归覆盖：

- `tests/result_error_aot.cmake`
- `tests/result_extended_aot.cmake`
- `tests/fixtures/result_*.luna`
- `tests/fixtures/never_type.luna`
- `tests/fixtures/panic.luna`
- `tests/runtime_abi_test.cpp`
- `tests/runtime_gpu_error_test.cpp`
- `tests/jit_aot_extended_parity.cmake`

新增错误能力必须同时覆盖成功、失败、move-only 清理、JIT/AOT 一致性和外部 ABI
边界；只增加语法正例不满足契约要求。

## 10. 编译器诊断编号

诊断采用 `error[阶段/编号]`，例如：

```text
error[ownership/OWN0001]: use after move of 'buffer'
```

编号可用于编辑器、CI 和文档检索；消息、源码片段和 `help:` 可以继续改进。

| 前缀 | 阶段 | 代表性编号 |
|---|---|---|
| `LEX` | 词法 | `LEX0001`：非法字符 |
| `PAR` | 解析 | `PAR0001`：缺少期望的语法元素 |
| `PKG` | 包加载 | `PKG0001`：输入不可读；`PKG0003`：包名不一致 |
| `SEM` | 类型/语义 | `SEM0001`：未定义名称；`SEM0002`：类型约束；`SEM0003`：缺失返回；`SEM0101`：C ABI |
| `TRT` | trait | `TRT0001`：trait 约束错误 |
| `OWN` | 所有权 | `OWN0001`：move 后使用；`OWN0002`：free 后使用；`OWN0003`：借用冲突；`OWN0004`：linear 未消费；`OWN0101`：GPU in-flight；`OWN0201`：控制流状态不一致 |
| `CGN` | 代码生成 | `CGN0001`：无效宿主 IR；`CGN0101`：CUDA；`CGN0102`：ROCm |
| `DRV` | 驱动/AOT | `DRV0001`：runtime 缺失；`DRV0002`：native linker 失败 |

每个阶段的 `9999` 是通用兜底码。新增公开细分类必须同时添加回归并更新本表。

## 11. 结构化编译器诊断

`luna check --message-format=json` 已实现 `luna.diagnostic` JSONL version 1。
协议顺序为 `hello`、零条或多条 `diagnostic`，最后一条 `summary`。诊断包含稳定的
severity、phase、code、message、可选 primary span、label、note 和 fix。当前渲染的
`help:` 会作为 note 输出；结构化 fix 字段已经保留，但尚不生成编辑。

磁盘文件的 primary span 使用规范化绝对路径、必填 UTF-8 byte offset、exclusive
end，以及从 1 开始的 line/column 展示辅助值。没有磁盘位置的诊断使用
`primary: null`。hello 会标识长期不变的语言版本、编译器源码 commit、构建 target
和协议 capability，因此工具无需只靠 `0.2.0-alpha` 推断编译器身份。
