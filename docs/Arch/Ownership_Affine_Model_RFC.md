# Luna 所有权、仿射与线性模型 RFC

> 状态：Accepted / Core implemented v0.2（2026-07-22）  
> 范围：Value Contract、Affine/Linear、Place、Borrow/Loan、控制流合并、MoonIR Cleanup  
> 非目标：任务、线程、channel、调度器以及其他并发运行时设计

## 1. 决策

Luna 不再用单个 `isLinear` 同时表示所有权、可复制性和销毁义务。值契约由两个正交维度组成：

```text
Ownership Relation = Owned | SharedBorrow | MutableBorrow
Usage Cardinality  = Copy | Affine | Linear
```

`move` 是从一个 Place 向另一个所有者转移值的操作，不是第四种 Usage。

- `Copy`：读取和传值不会使原 Place 失效。
- `Affine`：至多消费一次；允许未使用，并在拥有资源时执行确定性清理。
- `Linear`：必须恰好消费一次；离开任何可达路径时仍存活即为错误。
- `SharedBorrow`：允许重叠的只读 loan，不拥有源值。
- `MutableBorrow`：在重叠范围内具有唯一访问权，不拥有源值。

借用本身使用 `Copy` cardinality。被借用对象可能是 affine 或 linear；这不把借用句柄变成资源所有者。

## 2. 源码语法

```luna
affine let token: i32 = 1;       // 可丢弃，但不能隐式移交所有权
linear let resource = acquire(); // 每条退出路径都必须消费

fn inspect(value: Resource) -> i32;                  // move-only 类型默认只读 view
fn take(affine value: Resource) -> affine Resource;  // 显式拥有型传参与返回
fn close(linear value: Resource) -> i32;             // 必须消费的拥有型传参
fn read(value: &Resource) -> i32;                    // 显式 shared borrow
fn write(value: &mut Resource) -> i32;               // 显式 mutable borrow
```

`linear T` 与 `affine T` 是语义限定，物理值类型仍为 `T`。限定进入 callable shape、MoonIR 函数契约和模块验证，不进入 LLVM 标量类型。

初始规则保留 Luna 的低摩擦只读参数：未限定的 move-only 值参数是 shared view；`affine`/`linear` 参数才取得所有权。拥有型实参必须写出 `move`：

```luna
let next = take(move current);
```

这避免调用点发生不可见的所有权转移。

## 3. 默认 Usage

| 类型或声明 | 默认 Usage | 说明 |
|---|---:|---|
| 整数、浮点、布尔、普通 raw pointer | Copy | 读取不会使源失效 |
| heap struct/record/enum、string | Affine | 拥有值可以自动清理，不要求显式消费 |
| `device_buffer<T>`、`event` | Linear | 必须显式释放或 await |
| `&T`、`&mut T` | Copy | 是非拥有 loan；访问能力由 Relation 限制 |
| 显式 `affine T` / `linear T` | Affine / Linear | 覆盖默认 cardinality |

泛型实例化必须保留形参和返回值的显式 Usage。若泛型要转发一个 move-only 值，应显式声明：

```luna
fn identity<T>(affine value: T) -> affine T { return value; }
```

## 4. Place 模型

检查器追踪存储位置而不是只追踪变量名：

```text
Place := root (field | constant-index | dynamic-index | dereference)*
```

示例：`value`、`value.field`、`value[3]`、`value[*]`、`pointer*`。

两个 Place 在根相同且公共投影前缀不冲突时重叠。动态索引 `[*]` 与同一数组的任何索引保守重叠。因此：

- move `pair.left` 后仍可访问 `pair.right`；
- 不能访问整个 `pair` 或再次访问 `pair.left`；
- 可以同时借用 `pair.left` 和 `pair.right`；
- `borrow pair.left` 与 `borrow mut pair.left` 冲突；
- 对根、父投影或动态索引的 loan 会覆盖相应子 Place。

移动 Copy 字段是复制，不产生失效状态。移动 affine/linear 字段只失效该投影；直接字段全部移动后，聚合根被视为已移动。

## 5. 控制流与退出义务

所有权状态是控制流数据流的一部分。当前 frontend 在结构化 AST 上建立快照并按可达 fallthrough 合并；MoonIR 接收已验证的结果。合并状态包含：

- 根的 Valid/Moved/Freed 状态；
- 已移动的 Place 集；
- 活跃 loan 及 mutability；
- GPU in-flight loan；
- fragment/slot 的控制效果。

只有能到达后继语句的路径参与合并。`return`、`abort` 和 continuation 结束路径在各自退出点独立验证。循环体必须保持循环外资源状态，因为循环可能执行零次或多次。

Linear 值在普通作用域退出、return、fragment return/abort 的每条路径上都必须已经消费。Affine 值可以未使用；拥有的 heap affine 值形成 cleanup obligation。

## 6. MoonIR 契约

MoonIR 保存：

- 每个参数的 `Relation + Usage`；
- 返回值的 owning Usage；
- binding/call 的 Usage；
- callable canonical shape 中的完整 ownership contract；
- return 路径的显式 `CleanupObligation(place, action, TypeId)`。

MoonIR verifier 拒绝：

- 与兼容 `isLinear` 位不一致的 Usage；
- 非 Copy 的 borrowed parameter；
- 缺少 Place、重复或缺少冻结 TypeId 的 cleanup；
- call result 与声明不一致的 linear 标记。

LLVM lowering 只能在 MoonIR 验证后擦除这些语言级限定。

## 7. 付费边界

- Copy 标量不生成所有权运行时代码。
- borrow/Place 冲突完全在编译期检查。
- Affine 只有在确实拥有需清理的值时生成 cleanup。
- Linear 不引入引用计数；它只增加静态路径义务。
- 不使用 heap、GPU 或动态能力的程序不会因此链接新 runtime 组件。

## 8. 当前实现边界

本 RFC 已覆盖语言当前拥有的浅层 heap aggregate、raw owner、device buffer、event、结构化分支/循环和 fragment 控制流。用户定义 destructor、递归聚合的逐字段 drop glue、异常 unwind 以及 suspension point 不在当前语言核心中；引入这些能力时必须扩展 MoonIR cleanup action，而不能绕过本模型。

并发设计被明确排除。未来任何 task/channel/suspend 设计必须复用这里的 Place、Relation、Usage 和 cleanup obligation，不能另建一套所有权规则。
