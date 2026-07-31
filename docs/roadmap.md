# Luna roadmap / Luna 路线图

> 文档类别：项目路线图
> 适用版本：0.2 Alpha 之后
> 状态：Planned
> 规范性：非规范
> 更新：2026-07-31

This is the single active roadmap. The Chinese sections are authoritative;
English readers can use the summary below. Planned syntax is not a statement of
current compiler support.

## English summary

Before Beta, Luna will prioritize:

1. closing the documented type, ownership and error semantics;
2. splitting the compiler without changing the Alpha semantic baseline;
3. defining safe standard-library adapters over Runtime and C FFI;
4. extending heterogeneous compute without weakening explicit target/runtime boundaries;
5. evolving fragment plugins only after continuation lifetime and capability rules are proven;
6. adding package integrity, tooling and diagnostics before any remote registry.

Concurrency, broad dynamic reflection, hot replacement and remote dependency
resolution remain later work.

## 当前原则

- 当前能力以 [0.2 Alpha 语义参考](reference/README.md) 为准。
- 路线图中的语法、类型和 API 默认都未实现。
- 先补契约、负例和迁移说明，再扩大语言表面。
- Runtime、Dynamic 和硬件能力必须显式启用并具有可解释成本。
- 代码拆分只能移动实现职责，不得顺便改变冻结语义。

## 近期：文档与编译器结构

1. 维持类型系统、内置类型和错误模型的单一权威来源。
2. 建立实现模块地图，先拆分 driver，再拆分 CodeGenerator。
3. 最后拆分 SemanticAnalyzer；每次移动都保持 JIT/AOT 和语义回归。
4. 为 Parser、Sema、MoonIR、Codegen 和 Runtime 建立稳定内部接口。

## Beta 前：语言语义闭合

- 完成 `From`、Drop、递归类型和泛型边界的明确规则。
- 继续补齐 Place、部分移动、借用和控制流合并负例。
- 保持 `Result`/`?` 为显式可恢复失败，panic 保持 abort 边界，除非另有完整 RFC。
- 将标准库类型和编译器内置类型持续分离。
- 让 diagnostics 的稳定编号覆盖新增核心错误。

## 标准库与外部边界

- 在 `core`、`alloc/host`、`sys` 之间建立明确依赖方向。
- 用安全 adapter 包装 Runtime status、GPU error、errno 和 foreign resource。
- 通用堆拥有容器必须等待 Drop、allocator domain 和异常路径清理闭合。
- 扩大 C struct/union 和 callback FFI 前先定义布局、生命周期和线程边界。

## 异构计算

1. 保持 `--gpu-target` 与 `LUNA_GPU_BACKEND` 分离。
2. 扩展设备标量、buffer 和 grid 表面，同时维护 CPU simulator 一致性。
3. 对 CUDA/ROCm 增加更多硬件矩阵、长时间测试和可复现性能基线。
4. 在语言层 profiling API 稳定前继续使用显式 runtime/benchmark 工具。

## Fragment、Runtime 与 Dynamic

- 外部 plugin v1 继续限制为 host-only、single-shot interceptor。
- plugin v2 必须显式接收授权的 module context。
- 外部 context/resume、多发射和捕获必须先解决持久 continuation frame 与占用证明。
- Runtime Descriptor、registry、unload 和 re-select 必须具有显式生命周期。
- Dynamic reflection、inspect、replace 和 runtime weaving 不进入 Beta 核心，除非先完成
  capability、安全和回滚模型。

## Package 与工具链

- 增加内容摘要、缓存、签名和可复现构建验证。
- 远程 registry 和网络依赖解析晚于本地 workspace/lock 完整性。
- 改进 formatter、language server、诊断源码片段和测试选择。
- 发布流程继续使用严格警告、ASan/UBSan、安装树 JIT/AOT 与平台 CI。

## 后续能力

语言原生并发、task-local failure、通用 async、完整运行时反射、热替换和分布式
package 解析属于 Beta 之后的独立设计，不应通过小型补丁偷偷进入 Alpha。

## 交付规则

每项路线图工作必须同时说明：

- 所属层级和静态/运行时成本；
- 类型、所有权、错误和 ABI 影响；
- 正例、负例及 JIT/AOT 或硬件证据；
- 文档权威来源和迁移策略。
