# Luna Alpha 之后的发展路线

这份路线图描述 Alpha 之后的优先级，不把尚未实现的能力包装成当前版本承诺。
排序原则是：先固定安全边界和可观测性，再扩展性能与生态；每个阶段都必须有
正例、负例、JIT/AOT 一致性测试和迁移说明。

## 1. 加强异构计算

### 目标

让 GPU/加速器代码拥有清晰的地址空间、内存所有权、队列和同步语义，而不是
把后端 API 直接暴露给用户。

### 顺序

1. 把当前 `device_buffer<i32>` 扩展为受约束的 `device_buffer<T>`，明确
   host/device/shared 三种内存空间以及复制方向。
2. 引入显式 `device fn`、`kernel fn` 和统一的 `queue`/`event` 类型；
   `launch` 返回线性 event，未等待或未转移都必须被检查。
3. 让 grid、block、wave/wavefront 和共享内存成为可选的结构化 launch 配置，
   保留安全默认值，避免把硬件占用参数散落在普通表达式中。
4. 后端优先级：继续维护 CPU simulator、ROCm 和 CUDA；随后评估 Vulkan/SPIR-V
   与 WebGPU。每个后端都必须实现同一 host-side error/event ABI。
5. 增加 kernel ABI 版本、参数地址空间属性、别名信息和 occupancy 报告，减少
   Luna AOT 与 HIP/CUDA 编译器之间的调度差距。

### 不变式

- device 函数不能捕获 host continuation、抛出 host slot effect 或访问未经声明的
  host 地址。
- device buffer 的释放、复制和 in-flight event 生命周期必须由所有权检查器闭环。
- 后端不可用时必须显式失败，不能静默退回另一个后端。

## 2. 片段与槽升级

### 目标

在保留 `interceptor`、`context`、`resume()`、`abort()` 清晰语义的前提下，支持
真正动态的插件和更灵活的织入。

### 顺序

1. 完成持久化 continuation frame ABI：显式保存参数、所有权状态、返回槽和
   cleanup 状态，禁止保存借用栈地址。
2. 在 ABI v2 中支持外部 `context` 和 `resume()`；插件必须声明 single-shot 或
   replay-safe many，并通过逃逸/所有权校验。
3. 增加 manifest、slot contract hash、版本选择、能力声明和插件来源校验，支持
   运行时注册、卸载和可控热更新。
4. 引入结构化动态 effect：编译器能区分静态内联、动态单发射、动态多发射和
   不可优化边界；用户可用 `dynamic` 明确承担间接跳转成本。
5. 研究 slot 参数的命名绑定、结果传递和 typed `resume(args)`，但只有在不会
   破坏线性资源闭环时才进入语言表面。

### 不变式

- 外部插件不能获得未声明的捕获变量或裸 continuation 指针。
- `abort()`、`return`、`resume()` 的路径必须在所有分支上通过同一 ownership merge。
- 动态织入失败必须是可诊断的运行时错误，不能落入未定义行为。

## 3. 原生并发原语

推荐从结构化并发开始，而不是先暴露自由线程和任意共享可变状态。

### 第一批表面

```text
task = spawn(work(arg));
result = join(move task);
send(move value, channel);
value = receive(channel);
```

- `spawn` 只接受拥有所需数据的闭包；借用局部变量不能跨越 spawn 边界。
- `task<T>` 和 `channel<T>` 是线性句柄；必须 `join`、显式取消或在作用域结束时
  走可证明的 cleanup 路径。
- `send` 转移值所有权，`receive` 重新获得所有权；共享只通过受检查的只读引用。
- `select`/超时/取消令牌用于组合等待，取消必须是合作式的，不能强行破坏任意
  临界区。

### 后续原语

- `atomic<T>`：只允许满足原子布局和 `Send` 约束的类型。
- `mutex<T>` / `rwlock<T>`：锁守卫是线性值，解锁由消费守卫完成。
- `Send` / `Sync` trait：把跨 task 传递和跨线程共享能力变成显式类型约束。
- 运行时先提供固定线程池和 work-stealing，再评估 async executor；不把 OS 线程、
  fibers 和 GPU queue 混成一个没有边界的抽象。

### 必须先解决

任务 panic/错误传播、join 超时、取消期间的线性资源归还、线程退出时的 cleanup，
以及 C FFI 回调进入并发运行时的线程注册协议。

## 4. 更多 C FFI

1. 完善 `extern "C"` 类型映射：固定宽度整数、浮点、C 字符串、数组指针、
   opaque handle 和 ABI 对齐属性。
2. 用 `own`, `borrow`, `nullable`, `out`, `inout` 等显式契约标注外部参数；
   未标注的裸指针保持不安全并要求显式 unsafe 边界。
3. 支持 C struct/enum/union 的声明导入和布局检查，优先接入 clang AST/header
   importer，而不是重新实现完整 C 预处理器。
4. 增加安全 callback/trampoline：回调生命周期由线性 token 管理，线程进入、
   异常、错误码和 errno 传播规则固定下来。
5. 评估 C++、Rust、Fortran 等适配层，但不直接承诺跨语言 ABI 等价；它们应建立
   在稳定 C ABI 或独立 bindgen 工具之上。

## 5. 标准库雏形

### core/no-std 层

`Option`、`Result`、整数/浮点操作、`slice<T>`、迭代器、比较、基础错误和
panic/abort 策略。该层不能隐式依赖操作系统。

### alloc/host 层

`String`、`Vec<T>`、`HashMap<K,V>`、格式化、路径和字节缓冲区。所有拥有容器都
遵循统一 Drop/线性消费协议，并提供切片借用。

### sys/concurrency 层

文件、socket、时间、环境变量、线程、task、channel、锁和原子操作。平台差异
通过明确的 target module 暴露，不在类型检查器里散落平台分支。

### 交付方式

标准库与编译器版本绑定，使用独立 package manifest 和 lockfile；核心库每加入
一个容器或并发原语，必须同步加入所有权、借用、异常路径和 JIT/AOT 回归。

## 6. 工具链、生态与可观测性

- 包管理：manifest、依赖图、lockfile、缓存、镜像和供应链校验。
- 工具：formatter、语言服务器、增量编译、符号化诊断、源码级调试信息。
- 编译器：增量查询、并行代码生成、缓存 monomorphization、稳定 plugin ABI。
- 性能：CPU/GPU profile API、分层基准、IR/ISA dump、编译时间统计和回归阈值。
- 可靠性：fuzz parser/sema、sanitizer、模型检查 slot/ownership、跨后端差异测试。
- 发布：语义版本、迁移指南、兼容性矩阵、SBOM 和可复现构建。

## 阶段出口

### Beta 之前

- 稳定 `Option`/`Result`、slice 和一套最小 host 标准库。
- 完成 task/channel/join 的结构化并发 MVP。
- 外部 fragment context ABI 通过持久化 frame、逃逸和所有权测试。
- 至少一个额外异构后端完成同一套错误/事件/内存测试。

### Beta

- 包依赖与 lockfile 可复现。
- C FFI header importer 与 callback 生命周期稳定。
- 标准库、工具链和性能基线可供外部项目使用。
- 实验性语义有明确迁移方案，稳定核心拥有兼容性测试。
