# Result、错误传播与 Panic RFC

状态：实验性实现，作为协程和标准库之前的错误处理基础。

## 1. 决策

Luna 将失败分成两个互不混淆的边界：

- 可恢复失败使用普通值 `Result<T, E>`；
- 不可恢复的进程级失败使用 `panic(message)`。

`Result`、`?` 和 `panic` 不构成完整代数效应系统。语言不会因此引入 effect
声明、effect row/summary、handler、continuation 捕获或可恢复的 `perform`。
片段与槽仍负责受限动态扩展和结构化控制；错误值不越过片段/槽边界隐式传播。

sysmeta 继续作为编译器有权写入、用户至多只读的派生事实层。错误处理不会再建立
一套与 sysmeta 平行的 effect summary。sysmeta 描述实现和安全所需的控制、
资源、ABI 与 capability 事实，但不冒充用户可编程的 effect calculus。

## 2. `Result<T, E>`

`Result<T, E>` 是编译器内建的结构化和类型化 tagged union。`T` 是成功载荷，
`E` 是错误载荷；错误类型没有隐式父类，也不要求统一的 `Error` trait。

当前构造和查询接口为：

```luna
Ok(value)
Err(error)
Ok::<T, E>(value)
Err::<T, E>(error)

is_ok(result)
is_err(result)
unwrap(result)
unwrap_err(result)
```

上下文足够时，`Ok`/`Err` 的另一侧类型由约束求解器推导；上下文不足时可写显式
`::<T, E>`。`unwrap` 和 `unwrap_err` 在 variant 不匹配时调用 `panic`，因此只
适合断言已经建立的程序不变量，不是常规错误处理接口。

### 所有权

`Result` 拥有且只拥有当前 tag 对应的载荷。它的 usage 是两个载荷 usage 的上确界：

- 两侧均为 `Copy` 时，`Result` 为 `Copy`；
- 任一侧为 `Affine` 且都不是 `Linear` 时，`Result` 为 `Affine`；
- 任一侧为 `Linear` 时，整个 `Result` 为 `Linear`。

清理必须读取 tag，并且只对活动载荷执行一次对应的 Drop、unique deallocation、
`rc` release 或 `arc` release。不能同时清理两侧，也不能把活动载荷当作无主位串。
仿射 `Result` 传给消费型操作时仍要求显式 `move`。

## 3. `?` 的精确语义

表达式 `value?` 要求：

1. `value` 的类型是 `Result<T, E>`；
2. 当前普通函数返回 `Result<U, E>`；
3. 两个 `E` 当前必须是同一语义类型。

成功分支产生 `T`。错误分支在当前函数内执行与 `return` 相同的路径敏感清理，
然后直接返回原错误 Result 的 ABI 值。初始版本不执行隐式 `From`/转换；后续若
增加错误转换，应以普通静态 trait 调用表达，而不是扩展成 effect handler。

`?` 不允许出现在 fragment 中。fragment 的 `return`/`abort` 终止的是 fragment，
而不是其宿主函数；允许 `?` 穿过这条边界会同时模糊控制所有者和清理所有者。
fragment 必须显式检查 Result，并选择自身的 `resume`、`abort` 或普通完成行为。

MoonIR 为 `?` 保存已验证的 Result/value/error TypeId 和错误分支 cleanup
obligation。LLVM 后端不能重新猜测所有权状态。

## 4. `panic`

`panic(message)` 接受 `string` 或 `cstr`，是已知不返回的控制终点。当前实现采用：

- 输出 `Luna panic: <message>` 到 Runtime ABI 的 stderr console；
- flush；
- 调用进程 abort；
- LLVM IR 以 `unreachable` 结束。

初始策略明确不展开栈，因此 `panic` 不保证运行局部变量的 Drop。需要资源可预测
释放的失败必须使用 `Result` 和 `?`。未来若增加任务隔离或 panic capture，也必须
先定义任务边界、foreign frame、Drop 顺序和双重 panic；不能把现有 abort 语义
无声改成异常展开。

源码层暂未公开独立的 `never` 类型。Sema 已把直接 panic 语句识别为终止路径；
在通用发散表达式进入语言前，应先完成 `never` 的类型合一和不可达代码降低。

## 5. 当前 ABI 与限制

初始 Result ABI 是 `{ tag: i1, payload: i64 }`，整体 size/alignment 为 `16/8`。
它直接支持整数、布尔、浮点和一字指针表示的载荷；Luna 当前的 nominal
struct/enum、引用、字符串、unique、`rc` 和 `arc` 均使用指针表示。

数组、slice、嵌套 Result 等超过一字或 LLVM aggregate 的内联载荷尚未形成稳定
Result ABI。它们在进入稳定核心前需要改为按最大载荷大小和对齐推导的通用 tagged
union layout，并增加 JIT/AOT ABI 测试。当前一字 ABI 不应发布为跨 Moon 容器的
长期稳定外部 ABI。

## 6. 与后续协程的关系

协程可以让返回路径跨越挂起点，但不改变错误的含义：

- coroutine frame 必须保存仍存活的 Result/资源及其 drop flag；
- `?` 在异步函数中仍是“清理 frame 中当前活跃资源并完成为 Err”；
- panic 仍采用明确的任务或进程终止策略；
- 片段/槽不能因为协程存在而获得隐式错误 handler。

因此应先稳定 Result 的通用布局、错误转换 trait、panic 边界和 cleanup 验证，再
设计一等无栈协程关键字。
