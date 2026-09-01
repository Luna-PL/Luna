# Luna 0.3 开发期架构

> 文档类别：架构说明
> 适用版本：Luna 0.3.0 开发期
> 状态：Active
> 规范性：非规范；语言行为以当前语义参考与 0.3 设计为准
> 实现核对：2026-08-09

本文只描述当前系统的层级、编译管线和组件边界。活跃设计理由与实现完成门集中在
[0.3 总体设计](luna_0.3_design.zh-CN.md)；旧架构决策仅作为 0.2 理由基线。

## 设计原则

- **Static first**：能在编译期证明和消除的工作不推迟到运行时。
- **Pay for what you use**：Runtime、Reflection、Registry、Dynamic 和 GPU
  能力都必须由可达程序显式触发。
- **稳定具名身份、显式 shape**：具名声明默认名义；匿名 record 与显式 ShapeId 关系保留结构化编程。
- **Ownership 与 usage 正交**：Owned/Borrow 描述关系，Copy/Affine/Linear
  描述使用次数。
- **Lowest capable layer**：策略尽量放在能够安全实现它的最低层。
- **Specification / implementation separation**：参考文档定义行为，本文件解释组件。

## 系统层级

| 层级 | 当前职责 | 不承担的职责 |
|---|---|---|
| Compiler | 解析、类型与 trait 检查、所有权证明、MoonIR、LLVM 降低 | 通用运行时策略 |
| Core | 编译器认可的最小语言协议和内建表面 | 平台服务和容器策略 |
| Standard Library | 可替换的类型、算法、错误和平台 adapter | 隐式改变语言语义 |
| Runtime | 分配、console/filesystem host I/O、GPU、插件和错误快照的版本化 ABI | 完整语言反射 |
| Dynamic | 显式的运行时发现、选择和插件扩展边界 | 默认进入普通程序 |

“Runtime”和“Dynamic”是能力层级，不是所有值自动拥有的对象模型。普通程序没有
使用对应能力时，不应为 registry、descriptor 或动态分派付费。

Runtime 内建默认保持最小。编译器生成的 application entry 会显式请求 native
stdin/filesystem profile；embedding host 已安装的 service table 具有优先权。library module
不会仅因被链接就取得 host capability。

## 编译管线

```text
source/package
    -> Lexer / Parser
    -> SemanticAnalyzer / TraitChecker / OwnershipChecker
    -> verified MoonIR
    -> MoonIR optimizer
    -> LLVM lowering
    -> ORC JIT or textual LLVM IR + native AOT linker
```

MoonIR 是前端与后端之间唯一受支持的中间契约。进入 LLVM 前后均执行验证；JIT 和
AOT 消费同一份已检查 MoonIR，并共享宿主优化级别与 runtime ABI。

## 主要组件

阶段关系和数据流由本文定义；每个物理目录、文件的职责、允许依赖和禁止边界统一见
[仓库文件与职责指南](file_guide.md)。只重构文件不应跨越当前 Confirmed/TBD 边界。

## 关键身份

- **TypeId**：语言类型身份；名义类型不能仅因布局相同而合并。
- **ShapeId**：结构形状关系；不代替 TypeId。
- **Declaration identity**：package、module、声明族和 metadata 选择共同决定的声明身份。
- **Runtime ABI identity**：由版本化 C 结构和显式符号定义，不等同于源码类型身份。

精确类型规则见[类型系统参考](reference/type_system.md)；Runtime 边界见
[Runtime ABI](runtime_abi.md)。

## 静态与运行时选择

静态 selector 在编译期从类型正确的候选集合中选择声明；SF006 Slot/Fragment composition
同样是静态、名义的。Runtime-retained descriptor 仍是强类型 loader/tooling 数据；Luna 0.3
不公开 dynamic slot/apply 源码形式，也不接受任意原生函数指针或把环境变量当作安全边界。

Metadata 是声明附加数据和选择协议的扩展点；普通 metadata 不自动进入声明身份，
只有显式 selector/discriminator 规则可以区分声明族成员。

## 异构边界

`--gpu-target` 决定编译产物包含的设备代码；`LUNA_GPU_BACKEND` 决定程序执行时
选择的 backend。两者不能互相偷偷替代。当前可达 kernel 才生成设备代码和 runtime
入口，默认测试使用 CPU simulator，硬件 ROCm/CUDA 测试是额外门禁。

## 文档边界

- “现在是什么”：[`docs/reference`](reference/README.md)。
- “为什么这样决定”：[`decisions.md`](decisions.md)。
- “如何使用”：快速入门、CLI 和各专题指南。
- “以后做什么”：[`roadmap.md`](roadmap.md)。
- “实际改了什么”：根目录 `CHANGELOG.md`。

历史 Draft 和交接记录不保留在活动文档树中；需要追溯时使用 Git 历史。
