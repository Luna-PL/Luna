# Luna 0.3 开发期类型系统参考

> 文档类别：语言契约与开发期参考
> 适用版本：Luna 0.3.0 候选版
> 状态：活跃开发；具体表面按章节标注
> 规范性：类型域、身份、关系、usage 和形成规则规范；内部布局部分非公共 ABI
> 0.3 名义类型/record 实现检查点：2026-08-09

本文定义 Luna 当前类型系统的共同词汇和规则。所有源码类型的逐项状态见
[内置类型清单](builtin_types.md)。设计理由见
[架构决策 D003/D004](../decisions.md)。

## 1. 类型不等于布局，也不等于所有权

Luna 对一个值至少回答四个不同问题：

1. **类型**：它允许哪些值和操作；
2. **身份**：它与另一个类型是否语义相同、结构相同；
3. **ownership contract**：谁拥有它、可以消费几次；
4. **布局**：当前目标和 ABI 如何表示它。

两种类型可以布局相同但身份不同；同一类型可以在一个位置被拥有、在另一个位置被
借用；`move` 改变所有权状态但不创建新类型。

## 2. 类型域

| 类型域 | 含义 | 能否进入已验证运行时 MoonIR |
|---|---|---|
| Value | 普通执行值 | 可以 |
| Meta | Metadata schema/实例 | 默认只在编译期；显式保留时使用单独运行时编码 |
| Compiler | 声明视图、类型参数、融合 recipe 等编译器值 | 默认不可以 |
| Inference | 未求解类型变量 | 不可以 |
| Error | 诊断恢复占位 | 不可以 |

域是类型身份的一部分。一个 Meta schema 即使字段与 Value struct 完全相同，也不是
同一个类型。

## 3. 身份模式

| 模式 | 典型来源 | 身份规则 |
|---|---|---|
| Builtin | `i32`、`bool`、`never` | 由语言内置种类和参数确定 |
| Structural | 匿名 record、`Result`、函数 | 由完整的规范结构形状确定 |
| Nominal | 具名 `struct`/`enum`、trait | 由 package/module 声明身份确定 |
| MetaSchema | `meta` schema | 始终具有 schema 声明身份 |
| CompilerIntrinsic | 视图、type parameter、Iterator recipe | 由编译器契约确定，不是普通用户布局 |
| Inference/Error | Sema 状态 | 不形成可发布类型身份 |

### 3.1 TypeId

`TypeId` 表示语义类型身份。匿名结构类型的 TypeId 来自其规范结构；所有具名
struct/enum 的 TypeId 包含声明身份和泛型实参。

### 3.2 ShapeId

`ShapeId` 描述结构形状。它包括：

- 字段/variant 名称和规范顺序；
- 字段和载荷类型；
- 泛型实例参数；
- 引用的共享/可变类别；
- 函数参数与返回；
- callable 参数/返回的 relation 与 usage；
- slot/fragment 的控制种类和 Once/Many cardinality。

忽略名义 brand 后形状相同，不代表可以隐式赋值。

### 3.3 ABI compatibility

`type_abi_compatible` 当前是保守的、目标无关的兼容关系。它不能代替 TypeId，也
不能授权越过名义、FFI 或所有权边界。最终机器兼容仍受目标 data layout 和版本化
ABI 约束。

## 4. 源码类型类别

### 4.1 标量和特殊内置类型

整数、浮点、`bool`、`string`、`cstr`、`unit`、`never` 和 `event` 由编译器直接
识别。完整宽度、usage 和布局见[内置类型清单](builtin_types.md)。

### 4.2 参数化内置类型

当前源码可形成：

```text
raw<T>
&T
&mut T
rc<T>
arc<T>
array<T, N>
slice<T>
Result<T, E>
device_buffer<T>
(P1, P2, ...) -> R
```

`affine T` 和 `linear T` 可以出现在绑定、参数、返回和 callable contract 位置，
但修饰的是 usage，不是 `T` 的 TypeId。

`affine { ... }` / `linear { ... }` 为新建 local、match pattern 与 loop binding 提供
词法 default。普通嵌套 block 继承 default，嵌套 usage block 替换它，lambda body 从 Copy
重新开始。`copy let` / `affine let` / `linear let` 是显式 local 覆盖。Sema 将
default 与类型/initializer 固有要求合并，拒绝显式弱化，并且只向 MoonIR 输出最终
per-binding contract；不存在 usage-scope 运行时状态。

### 4.3 用户声明类型

- `struct`：具名名义 product；
- `enum`：具名名义 sum；
- `{ x: T, y: U }`：匿名结构 record 类型；
- `trait`：始终声明身份；
- `meta`：始终 MetaSchema 身份。

匿名 record 故意不使用 `record` 关键字。字段必须唯一，identity 和 layout 按字段名
规范化；`{ y: value, x: other }` 仍按源码顺序求值。`Point { x: value, y: other }`
显式构造具名 product，`{ x: point.x, y: point.y }` 显式投影为匿名 record。这些形式
之间以及不同具名声明之间都不发生隐式转换。

### 4.4 控制和编译期类型

slot、fragment、Metadata view、declaration view/ref 和编译器 Iterator recipe
具有专门契约。它们不能因为在内部 `TypeKind` 中有一项就被当成普通可存储数据。

## 5. 类型形成规则

### 5.1 命名和泛型类型

命名类型先解析当前类型参数和 `Self`，再解析 package/module 可见声明，最后匹配
内置名称。泛型实例必须提供声明要求的实参；实参参与最终类型身份。

具名 `constraint` 声明仍是编译期 proposition 机制。下列函数写法都归一为 constraint
检查，不会形成 effect 或运行时 contract：

```luna
fn first<Cartesian T>(value: T) -> i32 { ... }
fn second<T>(value: T) -> i32 where Cartesian<T> { ... }
fn third<T>(value: T) -> i32
where type_same_shape::<T, { x: i32, y: i32 }>() { ... }
```

inline 形式是匿名前端谓词，没有声明身份，且在 MoonIR 之前擦除。`where T: Tagged`
这样的 trait bound 仍是行为要求，不是结构 constraint。

### 5.2 引用

若 `T` 是已形成类型，则 `&T` 和 `&mut T` 是不同引用类型：

- `&T` 具有 shared-borrow relation；
- `&mut T` 具有 mutable-borrow relation；
- 两者不能通过形状统一互换；
- 引用不拥有 `T`，不能比来源 loan 活得更久。

### 5.3 数组与切片

`array<T, N>` 要求：

- 恰好一个元素类型；
- `N` 是源码中的编译期非负整数；
- 长度属于类型身份和结构形状。

`slice<T>` 是当前只读、非拥有 `{data, length}` 视图。创建 slice 会对来源建立
共享 loan；`raw<T>`、`device_buffer<T>` 不会隐式变成 slice。

### 5.4 Result

`Result<T, E>` 恰好有两个可用于普通运行时值位置的载荷类型。它是结构 sum；其
活动载荷、usage、匹配和清理规则见[错误模型契约](error_model.md)。Sema 对
Compiler/Meta 载荷的完整 well-formedness 负例矩阵仍是 A0 后的实现核对项。

### 5.5 callable

闭包/函数类型写作：

```luna
(i32, &string) -> bool
(affine Resource) -> affine Resource
```

参数和返回类型、relation 与 usage 都属于 callable 的语言级 shape。后端只有在
MoonIR 验证后才能从机器调用约定中擦除不需要的静态信息。

### 5.6 递归

第一版拒绝无限 inline 递归结构。递归必须跨越编译器认可的表示边界，例如名义
pointer-represented product、引用、`raw`、`rc` 或 `arc`。结构相等和布局计算不得
因递归进入无限展开。

## 6. 推断与 `auto`

`auto` 是“在此创建推断变量”的源码请求，不是可以反射、存储或传递的类型。

Sema 使用 Inference 域变量收集约束：

- 相等/赋值/参数/返回产生统一约束；
- 数值运算产生 numeric 约束；
- 条件和逻辑运算产生 bool 约束；
- 未受其他约束的数值推断变量默认到 `i32`；
- 仍未求解的变量产生诊断；
- MoonIR Verifier 拒绝 `InferenceVar` 和 `Unknown`。

错误恢复中用 `i32` 或 `Unknown` 继续解析，不代表错误程序获得了有效类型。

## 7. 字面量与转换

### 7.1 默认类型

| 字面量 | 默认类型 |
|---|---|
| 整数 | `i32` |
| 浮点 | `f64` |
| `true`/`false` | `bool` |
| 字符串 | `string` |

### 7.2 当前上下文规则

当前调用参数检查允许整数常量在已知 numeric 参数位置按目标宽度生成；当前没有
完整范围诊断。字符串字面量可以在已知 `cstr` 的绑定、参数和返回位置使用。

这些是字面量的上下文表示规则，不是：

- 任意整数类型之间的隐式转换；
- 任意 `string` 到 `cstr` 的隐式转换；
- 用户值的通用 cast；
- ABI 兼容即类型兼容。

### 7.3 普通数值运算

- `+ - * / %` 要求 numeric 操作数，并统一为同一类型；
- `& | ^ ~` 要求 integer；
- shift 的左值和计数都必须为 integer，结果类型取左值；
- `< <= > >=` 要求同一 numeric 类型，结果为 `bool`；
- `&& || !` 要求 `bool`；
- `== !=` 要求两个操作数可统一，结果为 `bool`。

Luna 不承诺一般性的隐式数值提升。需要新增转换时必须先定义溢出、截断、符号和
constexpr 行为。

## 8. relation 与 usage

### 8.1 relation

| relation | 含义 |
|---|---|
| owned | 当前位置承担消费/清理责任 |
| shared_borrow | 只读 loan，不承担释放责任 |
| mutable_borrow | 独占可写 loan，不承担释放责任 |

### 8.2 usage

| usage | 含义 |
|---|---|
| Copy | 可以重复使用 |
| Affine | 至多消费一次；未消费时可以由作用域清理 |
| Linear | 必须在每条可达路径恰好转移、等待或消费 |

### 8.3 默认参数规则

未显式标注的 Copy 参数是拥有 Copy 值。未显式标注的 move-only 参数保持共享借用
视图。要消费调用方值，参数必须显式写 `affine` 或 `linear`，调用点必须进行相应
`move`。

引用参数直接从 `&T`/`&mut T` 得到 shared/mutable relation；usage 为 Copy，
但 loan 生命周期仍受检查。

## 9. 清理与组合类型

清理责任来自拥有值的 usage 和资源管理方式：

- 独占 product/string：Drop/Deallocate；
- 匿名 record：递归清理其拥有型字段；纯 Copy record 不产生清理；
- `rc<T>`：RcRelease；
- `arc<T>`：ArcRelease；
- Result/enum：读取 tag 后只清理活动载荷；
- array：按仍初始化元素执行 ArrayDrop；
- event：必须 await 或合法转移；
- device buffer：必须显式释放或合法转移；
- 借用、裸 Copy 指针、标量：不释放来源资源。

组合 usage 使用 `Linear > Affine > Copy`。该规则不能因某条常见路径只使用 Copy
variant 而放宽。

当前 record 实现允许移动整个 record。在 record 字段初始化位能够证明其余字段只清理
一次之前，从匿名 record 中部分移出拥有型字段会被拒绝。

## 10. 当前布局层

布局分为三层：

1. **语言语义**：字段/variant 顺序、活动载荷和身份边界；
2. **当前编译器/MoonIR 开发期 ABI**：当前 64 位值大小、对齐和 inline ADT v1；
3. **公共 Runtime/C FFI ABI**：只包含显式版本化并允许穿过边界的类型。

当前具名 product 仍是 pointer-represented；匿名 record、array 和 slice 内联；
enum/Result 使用 8 字节 tag storage 和 8 字节对齐 payload。该指针表示只是
实现检查点，不是语言层承诺。具体数字见
[内置类型清单](builtin_types.md)。

这些数字不得自动推广到 32 位目标、跨版本 Moon 容器或 C ABI。`type_size` 当前
报告的是第 2 层事实。

## 11. 边界

### 11.1 C FFI

当前允许整数、浮点、`cstr`、`raw<T>`、`unit`，以及指向受支持标量的引用。
`bool`、`string`、product、enum、Result、shared handle、closure 和 device buffer
不属于当前 C ABI 表面。拥有型 FFI 返回只允许显式 `linear raw<T>`。

### 11.2 kernel

kernel 参数需要显式 ABI 类型；当前稳定设备表面主要是标量和
`&device_buffer<i32>`/`&mut device_buffer<i32>`。`string`、host allocation、
FFI、reflection、closure 和 host continuation 不得进入当前 device 子语言。

### 11.3 compile time

constexpr 当前处理标量字面量、不可变绑定、受支持表达式和反射结果。Compiler/Meta
值只有在编译器提供相应求值规则时才可参与，不获得默认运行时表示。

## 12. 标准库类型不是编译器内置类型

`org.luna.core` 中的 `Option<T>`、错误 enum、`Iterator`、`IntoIterator`、
`FromIterator`、`Map`、`Filter` 和 `Take` 都有 package/module 声明身份。

编译器可以识别唯一 Core trait 并静态融合，但不得把同形状的用户 trait 当成 Core
协议。内部 `TypeKind::Iterator` 表示融合 recipe，不等于
`org.luna.core::iter::Iterator` trait，也不会自动成为跨函数 ABI。

## 13. 实现映射与后续拆分

| 责任 | 当前主要实现 |
|---|---|
| 类型 AST/语法 | `src/parser/AST.h`、`src/parser/Parser.cpp` |
| 类型种类和构造 | `src/core/TypeSystem.h` |
| 推断/统一 | `src/sema/TypeSystem.cpp` |
| 声明解析和内建语义 | `src/sema/SemanticAnalyzer.cpp` |
| 身份与形状 | `src/core/TypeRelations.cpp` |
| relation/usage | `src/core/Ownership.h`、OwnershipChecker |
| 大小和对齐 | `src/core/TypeLayout.cpp` |
| 可信类型表 | MoonIR/Verifier |
| 机器表示 | CGHelpers/CodeGenerator |

后续代码拆分应围绕这些责任进行，并逐步建立集中内置类型注册表。已冻结的
[0.2 Alpha 语义基线](semantic_baseline_0.2.md)仅作为迁移证据，不是 0.3 编译器的兼容模式。
