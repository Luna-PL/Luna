# Luna 0.2.1 路线图（已取代）

> 文档类别：项目路线图
> 适用版本：冻结的 0.2.1 规划历史
> 状态：已由 `luna_0.3_design.zh-CN.md` 取代
> 规范性：非规范
> 取代日期：2026-08-09

本文仅保留原 0.2.1 路线图作为迁移背景，不再是活跃计划。当前实现顺序与完成门见
[Luna 0.3 总体设计](luna_0.3_design.zh-CN.md)。

## English summary

Luna is entering a long-lived `0.2.1` line with no scheduled Beta or
language-version bump. Near-term work prioritizes diagnostics, editor/build/
package integration, distribution, reproducibility, testing and developer
tools. Language changes are limited to correctness, safety, documented contract
gaps and internal interfaces required by tooling.

New language surface, concurrency, broad dynamic reflection, hot replacement
and remote dependency resolution are not active development goals.

## 当前项目状态

- `0.2.1` 是长期维护版本线；升级不会由时间自动触发。
- 当前开发主线是工具链，不是继续扩大语言表面。
- 同版本构建以源码 commit、发布 manifest 和校验和标识。
- Beta 或新语言版本必须另行决策，并重新执行语义基线与迁移审查。

## 当前原则

- 当前能力以 [0.2 Alpha 语义参考](reference/README.md) 为准。
- 路线图中的语法、类型和 API 默认都未实现。
- 只处理正确性、安全性、契约缺口或工具链所需内部接口，不主动扩大语言表面。
- Runtime、Dynamic 和硬件能力必须显式启用并具有可解释成本。
- 代码拆分只能移动实现职责，不得顺便改变冻结语义。

## 近期：工具链

1. 改进稳定诊断编号、源码片段、修复建议和机器可读输出。
2. 建立 formatter、language server 与编辑器最小集成。
3. 完善 build/test 命令、测试选择、package/workspace 工作流与本地缓存。
4. 稳定安装树、预编译包、校验和、可复现构建与跨平台发布。
5. 提供 benchmark、profiling、MoonIR/LLVM 检查和开发者审计工具。
6. 只在工具链边界需要时继续拆分编译器内部模块。

## 维护性工作

1. 维持类型系统、内置类型和错误模型的单一权威来源。
2. 依照[文件与职责指南](file_guide.md)维护实现边界。
3. 修复实现与冻结契约不一致，并补齐正例、负例和迁移说明。
4. 为工具链消费者稳定 Parser、Sema、MoonIR、Codegen 和 Runtime 的必要内部接口。

## 非近期语言工作

- 完成剩余 `From`、部分移动初始化和泛型特化边界；递归泛型 Drop 合约已实现。
- 继续补齐 Place、部分移动、借用和控制流合并负例。
- 保持 `Result`/`?` 为显式可恢复失败，panic 保持 abort 边界，除非另有完整 RFC。
- 将标准库类型和编译器内置类型持续分离。
- 让 diagnostics 的稳定编号覆盖新增核心错误。

## 标准库与外部边界

- 在 `core`、`alloc/host`、`sys` 之间建立明确依赖方向。
- 用安全 adapter 包装 Runtime status、GPU error、errno 和 foreign resource。
- 普通 Core Rc/Arc 已覆盖 Drop 和 allocator-domain cleanup；Vec/Box 等通用拥有
  容器仍必须等待 element initialization tracking、mutable-view 失效和可恢复失败路径闭合。
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
- Dynamic reflection、inspect、replace 和 runtime weaving 不进入当前稳定核心，除非先完成
  capability、安全和回滚模型。

## Package 与分发

- 增加内容摘要、缓存、签名和可复现构建验证。
- 远程 registry 和网络依赖解析晚于本地 workspace/lock 完整性。
- 改进 formatter、language server、诊断源码片段和测试选择。
- 发布流程继续使用严格警告、ASan/UBSan、安装树 JIT/AOT 与平台 CI。

## 后续能力

语言原生并发、task-local failure、通用 async、完整运行时反射、热替换和分布式
package 解析没有当前排期，必须作为未来独立设计，不得通过工具链补丁进入 Alpha。

## 交付规则

每项路线图工作必须同时说明：

- 所属层级和静态/运行时成本；
- 类型、所有权、错误和 ABI 影响；
- 正例、负例及 JIT/AOT 或硬件证据；
- 文档权威来源和迁移策略。
