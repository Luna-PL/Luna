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

match result {
    Ok(value) => { ... },
    Err(error) => { ... }
}
```

上下文足够时，`Ok`/`Err` 的另一侧类型由约束求解器推导；上下文不足时可写显式
`::<T, E>`。`unwrap` 和 `unwrap_err` 在 variant 不匹配时调用 `panic`，因此只
适合断言已经建立的程序不变量，不是常规错误处理接口。

`match` 是 enum 与 Result 共用、穷尽且封闭的结构化匹配；对 Result 必须各有一个
`Ok` 和 `Err` 分支，载荷绑定只在对应分支可见。`match move result` 转移
仿射/线性 Result 的所有权；普通 Copy Result 可以直接匹配。MoonIR 保留已验证的
tag 分派和载荷偏移，因此不增加 handler、动态分派或新的运行时控制机制。

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
2. 当前普通函数返回 `Result<U, F>`；
3. `E` 与 `F` 相同，或存在唯一的静态 `impl From<E> for F`。

成功分支产生 `T`。错误分支在当前函数内执行与 `return` 相同的路径敏感清理，
若错误类型不同则调用唯一选中的 `From::from`，随后按外层 `Result<U, F>` 的布局
重新构造 `Err` 并返回。即使 `E == F` 也会重建外层 Result，不能因为成功载荷
`T` 与 `U` 的尺寸可能不同而错误复用内层 ABI 值。

`From<Source>` 是编译器已知身份、用户提供实现的静态转换 trait：

```luna
impl From<ParseError> for AppError {
    fn from(error: ParseError) -> AppError { ... }
}
```

当前只接受具体 Source/Target、一个非泛型 `from(Source) -> Target` 方法；若
Source 是 move-only，参数必须显式写为 `affine` 或 `linear` 来取得所有权。解析按
精确 TypeId 查找且只允许一条直接边；不进行传递转换搜索，不使用 vtable，也不在
运行时查询注册表。这样可以避免转换链随导入集合改变、歧义和隐藏分配。泛型 From
impl 的显式静态调用语法仍待补充；跨包 orphan/coherence 已要求 impl package
拥有 trait 或目标名义类型，`From` 则要求拥有 Source 或 Target。

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

源码层公开 `never` 作为发散表达式的类型。`never` 没有可构造值，是所有普通值
类型的 bottom type；`panic` 和其他确定不返回的调用产生 `never`。Sema 用它完成
分支类型合一和返回路径判断，LLVM lowering 为不返回函数附加 `noreturn` 并以
`unreachable` 终止控制流。

## 5. 标准错误与外部边界

Core/Std 不应提供一个吞掉所有信息的全局错误枚举。库 API 返回最窄的具体错误，
应用可以用自己的 tagged union 汇总，并通过 `From` 接入 `?`。预定的层次为：

- Core：`InvalidArgument`、`BoundsError`、`UtfError` 等不依赖 host 的值错误；
- 分配：`AllocError`，只携带稳定原因码和请求布局，不隐式 panic；
- Std：`IoError`、`PathError` 等 host/OS 错误；
- 边界：`FfiError`、`RuntimeError`、`GpuError`，保留 domain、稳定 code，并可选
  拥有一份诊断文本，不能只借用易失的 `last_error` 指针。

Core 的值错误现已基于通用匹配和冻结的 inline ADT ABI 物化。Runtime ABI v1
也已为 GPU 与 external fragment plugin 提供 caller-owned
`domain/code/message` 快照，并规定分配失败时保留机器字段、允许省略诊断文本。
语言层 `FfiError`/`RuntimeError`/`GpuError` 及其安全 adapter 仍待物化；它们
不会借用易失的外部错误文本。

原始 `extern "C"` 只接受 C ABI 类型，明确拒绝直接传递 Luna `Result` 或标准错误
ADT。安全 adapter 是普通 Luna 函数：调用 raw FFI，立即读取 status/errno/错误
快照，复制所需诊断，再返回 `Result<T, FfiError>`。编译器不会猜测某个返回值是否
是 errno，也不会自动读取进程全局错误状态。

Runtime ABI 同样以稳定 status/domain/code 为机器判据，文本只用于诊断。
`rt_runtime_error_snapshot_v1` 已能复制最近一次 GPU/plugin 错误；具体可恢复操作
仍应逐步采用 status/out-parameter 返回，并由相应 Core/Std adapter 转成
`Result`。不能把可能失败的 C++ 异常越过 C ABI，也不能把
`rt_gpu_last_error()` 之类的借用字符串直接存进长期错误值。

详细边界见 [标准错误与 FFI/Runtime 转换](Standard_Error_Boundaries_RFC.md)。

## 6. 当前 ABI 与限制

Result 使用通用 tagged-union ABI：
`{ tag: i1, payload: [N x i64] }`，其中
`N = max(1, ceil(max(sizeof(T), sizeof(E)) / 8))`。tag 后的隐式 padding 使载荷
保持 8 字节对齐，整体 size 为 `8 + 8*N`、alignment 为 8。数组、slice 和嵌套
Result 均可作为内联载荷；指针表示的 nominal struct、引用、字符串、`rc` 和
`arc` 存放一个指针字。清理代码先读 tag，且递归销毁嵌套 Result 中唯一活跃的
载荷。

该布局目前是编译器与 MoonIR 共同验证的内部 ABI；在发布为稳定 FFI ABI 前，仍需
明确目标平台 data layout、非 64 位平台策略和更高对齐载荷的规则。

## 7. 与后续协程的关系

协程可以让返回路径跨越挂起点，但不改变错误的含义：

- coroutine frame 必须保存仍存活的 Result/资源及其 drop flag；
- `?` 在异步函数中仍是“清理 frame 中当前活跃资源并完成为 Err”；
- panic 仍采用明确的任务或进程终止策略；
- 片段/槽不能因为协程存在而获得隐式错误 handler。

因此协程语法、frame ABI 和关键字设计全部延后；至少要先稳定 Core 错误/Option/
Iterator、资源清理和 package trait 边界。当前错误模型不得为了预想中的 async
语法预埋隐式 handler 或 effect summary。
