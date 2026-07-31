# Luna 0.2 Alpha 语义基线

> 文档类别：语言契约
> 适用版本：Luna 0.2.0-alpha
> 状态：Frozen for Alpha
> 规范性：规范，但仅覆盖本文列出的类型、所有权和错误边界
> 首次实现核对：`d0ab31c`（2026-07-31）

本文是方案 A0 的语义冻结记录。冻结的含义是：在 0.2 Alpha 内，编译器重构和
文档整理不得静默改变这些规则。本文没有列出的实验性语言表面仍由各自 RFC 和
专题文档管理。

## 1. 冻结目标

A0 冻结“语言如何理解值、类型、所有权和失败”，不冻结所有 API 名称和机器表示。
它为后续文档建设、代码拆分和 Alpha 发布提供共同基线。

## 2. 类型模型

### 2.1 类型域

Luna 必须区分：

- **Value**：普通执行中的值类型；
- **Meta**：Metadata schema 及其实例的编译期类型；
- **Compiler**：声明视图、Metadata 视图、类型参数和编译器降级配方；
- **Inference**：尚未求解的 Sema 推断变量；
- **Error**：错误恢复占位。

`Inference` 和 `Error` 域不得进入已验证 MoonIR。Compiler 域的值只有在对应
特性定义了显式运行时编码时才能保留到运行时。

### 2.2 身份、形状与布局

Luna 必须分别处理：

- **TypeId**：语义类型身份；
- **ShapeId**：结构形状身份；
- **ABI/layout compatibility**：物理表示兼容性。

三者不得相互代替。布局相同不能越过名义身份；名称相同也不能绕过 package/module
声明身份。

`struct` 和 `enum` 默认采用结构身份；`nominal struct` 和 `nominal enum` 采用
声明身份。Trait 和 Metadata schema 始终具有声明身份。字段、variant、顺序、
子类型、引用可变性和 callable ownership contract 都参与结构形状。

### 2.3 类型与所有权正交

类型回答“值是什么”；ownership contract 由两个独立维度组成：

- relation：`owned`、`shared_borrow`、`mutable_borrow`；
- usage：`copy`、`affine`、`linear`。

`move` 是所有权状态转移，不是类型种类。`affine`/`linear` 修饰的是拥有值的使用
契约，不创造新的 TypeId。引用类型携带共享/可变借用关系，借用本身不接管来源值的
释放责任。

### 2.4 usage 推导

0.2 Alpha 必须遵守：

- 标量、裸指针、引用、`cstr`、函数值和无拥有资源的普通值默认为 Copy；
- 独占堆值、`string`、普通 product 实例、`rc<T>`、`arc<T>` 默认为 Affine；
- `device_buffer<T>` 和 `event` 默认为 Linear；
- 编译器 Iterator recipe 默认为 Affine；
- `array<T, N>` 的 usage 由 `T` 决定；
- enum 和 `Result<T, E>` 的 usage 是所有可能活动载荷 usage 的上确界：
  `Linear > Affine > Copy`。

显式函数参数/返回契约可以要求拥有的 Affine 或 Linear 值。未显式标注的 move-only
参数保持借用视图语义，不能静默消费调用方所有权。

### 2.5 类型检查与转换

- `never` 是普通值类型的 bottom type；确定发散的表达式可以满足任意普通返回
  位置；
- `auto` 是请求推断的源码标记，不是运行时类型；
- 未求解的推断变量必须在 MoonIR 前被拒绝；
- 普通算术、比较和位运算要求操作数满足对应类别并统一为同一类型；
- Luna 不提供一般性的隐式数值提升；
- 当前整数和浮点字面量分别默认为 `i32` 和 `f64`；
- 当前调用参数位置允许整数常量按已知数值参数宽度表示，字符串字面量可在已知
  `cstr` 位置使用；这两个上下文规则不能被推广成任意值转换。

完整规则见[类型系统参考](type_system.md)。

## 3. 错误模型

0.2 Alpha 冻结以下决策：

1. 可恢复失败是普通 `Result<T, E>` 值；
2. `?` 在 `Err` 路径执行与显式 `return` 相同的路径敏感清理；
3. 错误转换静态选择唯一、直接的 `From<Source> for Target`，不做运行时搜索；
4. `panic` 是明确不返回的进程 abort，不进行语言栈展开；
5. 错误处理不隐式创建异常、effect row、handler 或 continuation；
6. Luna `Result` 和标准错误 ADT 不直接穿过 C ABI；
7. Runtime/FFI 可恢复边界使用稳定机器字段，诊断文本只是可选的拥有快照。

错误模型的 API 状态和未冻结部分见[错误模型契约](error_model.md)。

## 4. MoonIR 信任边界

前端必须在 MoonIR 前完成：

- 类型形成和推断求解；
- TypeId/ShapeId 和名义身份解析；
- relation/usage 检查；
- move、借用和路径合并；
- 活动 ADT 载荷与 cleanup obligation；
- `?` 的错误转换符号和返回类型；
- host/device、FFI 和 compile-time 类型边界。

MoonIR 必须保存后端安全生成代码所需的已验证事实。LLVM 后端不得重新猜测 trait、
错误转换、所有权状态或清理责任。

## 5. 本基线没有冻结的内容

下列内容在 `0.2.0-alpha` 长期维护期内仍允许调整，但必须按文档规则标注：

- Result/enum 的跨版本公共 FFI 布局；
- 非 64 位目标和高于 8 字节对齐载荷的内部布局策略；
- 泛型 `From`、显式静态调用语法和错误 source 聚合 API；
- `FfiError`、`RuntimeError`、`GpuError` 的语言层 adapter；
- task-local panic、panic capture、协程 frame 和结构化并发；
- closure environment 和跨函数 Core adapter Drop 布局；
- Compiler Iterator recipe 的内部结构；
- 通用 `device_buffer<T>` 的设备操作和更多 kernel 类型；
- `String`、`Vec<T>` 等未来标准库容器 API；
- selector、外部动态 context 和多发射 continuation ABI。

“未冻结”不表示实现可以无文档变化。任何用户可观察变化仍必须更新状态矩阵、测试
和变更日志。

## 6. A0 实现核对结果

本基线首次与以下实现层核对：

| 层 | 当前事实 |
|---|---|
| Parser | 识别 usage 修饰、引用、函数类型、内置/命名/泛型类型语法 |
| Sema | 解析内置类型、推断、结构/名义声明、Result 和 compile-time 视图 |
| TypeRelations | 为类型身份和形状生成规范化描述 |
| Ownership | 独立记录 relation、usage 和 cleanup action |
| Layout | 计算当前编译器/MoonIR 值大小、对齐和 inline ADT 布局 |
| MoonIR Verifier | 拒绝 Unknown/Inference，并验证类型与清理事实 |
| Codegen/Runtime | 实现 JIT/AOT、Result 清理、panic 和 Runtime ABI v1 |

清点同时发现以下工程问题，留给代码拆分阶段处理：

- 内置名称解析分散在通用 `resolveType`、Sema 和部分内建调用逻辑中；
- `TypeKind` 混合语言类型、Compiler recipe、Inference 和 Error 状态；
- 类型打印、身份、布局、FFI 和 kernel 允许集合尚无集中注册表；
- callable ownership identity 已进入形状模型，但仍需要更直接的赋值/统一负例覆盖；
- Result、array 和 ownership wrapper 的类型域/well-formedness 仍需要统一的负例
  矩阵，避免 Compiler/Meta 类型误入普通 Value 容器；
- 当前布局引擎是 64 位 Alpha 模型，不能自动被解释为所有目标平台的公共 ABI。

这些问题不改变本文契约，但会指导后续代码拆分和可执行类型注册表建设。

## 7. 变更门槛

修改本文冻结规则必须同时：

1. 给出设计理由和受影响边界；
2. 更新类型/错误参考和稳定性矩阵；
3. 增加正例、负例以及必要的 MoonIR/JIT/AOT/ABI 测试；
4. 记录迁移方式；
5. 在 `CHANGELOG.md` 声明用户可观察变化。

纯代码拆分不得改变本文。若拆分暴露实现与本文不一致，应先记录缺陷，再单独决定是
修复实现还是修订下一版本契约。

