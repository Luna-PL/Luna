# 标准错误与 FFI/Runtime 转换 RFC

状态：Core 值错误已物化；FFI/Runtime 边界等待 owned diagnostic/status ABI。

## 1. 目标

标准错误模型必须同时满足：

- 可恢复失败是普通 `Result<T, E>` 值；
- 错误转换在编译期唯一确定、零动态分派；
- C/Runtime ABI 不暴露 Luna 私有 tagged-union 布局；
- 错误值不持有会被下一次 runtime/errno 操作覆盖的借用文本；
- 不为错误处理引入完整代数效应或 effect summary。

## 2. 类型分层

Core 只定义无 host 依赖的错误，如参数、边界、编码和分配布局错误。Std 定义 I/O、
路径、网络等 host 错误。FFI、Runtime 与 GPU 错误是边界错误，不作为所有库错误的
共同父类。

稳定的最小错误记录由以下信息组成：

| 字段 | 含义 |
|---|---|
| `domain` | Luna Core、OS、第三方库、Runtime、GPU backend 等稳定域 |
| `code` | 域内稳定整数码；不能用诊断文本参与程序控制 |
| `message` | 可选、拥有的诊断快照；无内存时允许缺失 |
| `source` | 由具体聚合错误类型决定，不要求全局装箱链 |

库 API 应返回最窄错误类型。应用级 `AppError` 通过多个精确
`impl From<SpecificError> for AppError` 汇总，不做运行时 downcast。

## 3. Raw FFI 到安全 adapter

原始声明只描述 C ABI：

```luna
extern "C" fn foreign_read(handle: raw<u8>, out: raw<u8>) -> i32;
```

普通 Luna adapter 负责：

1. 调用 raw 函数；
2. 在任何可能覆盖错误状态的调用之前立即捕获 status/errno；
3. 把外部 domain/code 映射成拥有的 `FfiError`；
4. 使用创建资源的同一 release capability 管理外来资源；
5. 返回 `Result<T, FfiError>`。

`Result`、Luna enum/struct、`string`、`rc`、`arc` 均不得直接穿过 C ABI。C++
异常必须在 C wrapper 内捕获并转换为 status；异常越过 `extern "C"` 是 ABI
违约。回调还需单独规定线程注册、panic 和借用寿命，不能从普通函数声明推导。

## 4. Runtime 边界

可恢复 Runtime API 使用 `status + out parameter` 或等价 C 结构；status 为零表示
成功，非零值由稳定 domain/code 解释。`last_error` 文本只是紧邻失败调用的借用
诊断视图，adapter 必须立即复制，且不能把它当唯一错误身份。

abort 型入口（`rt_panic_cstr`、当前 GPU operation abort reporter）只服务已判定为
不可恢复的边缘。标准库 API 不应为了复用这些入口而把可恢复 I/O、分配或设备错误
升级为 panic。

## 5. 与 sysmeta 的关系

编译器从声明和实现推导：

- Result/错误载荷的 usage 与 cleanup；
- `From` 调用的精确符号和 Source/Target TypeId；
- FFI capability、ABI 稳定边界和 ownership contract；
- panic/never 的不返回控制事实。

这些是只读 sysmeta 事实，不是用户编写的 effect 标注，也不存在单独的 effect
summary。MoonIR 必须保存转换符号、两侧错误类型和清理义务，后端不得重新搜索
trait 或猜测 errno。

## 6. 物化门槛

Core 值错误已满足前三项并发布；FFI/Runtime/GPU 边界错误仍须完成后两项：

1. [x] 通用 enum variant 匹配；
2. [x] 稳定 inline ADT layout；
3. [x] 跨包 trait coherence/orphan 规则；
4. [ ] owned diagnostic string 的 OOM 策略；
5. [ ] Runtime status/domain/code 快照 ABI 与 JIT/AOT 测试。

协程不在这些前置条件中，也不会在此阶段设计。
