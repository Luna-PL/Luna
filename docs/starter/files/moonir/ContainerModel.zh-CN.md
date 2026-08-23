# src/moonir/ContainerModel.cpp

实现 ContainerModel.h 的全部编解码方法：用 Encoder/Decoder 状态机把 MoonIR 的 Module/声明表/CFG 翻译成 8 个 section 的字节，并执行"先验证、后发布"的原子装载。

## 这个文件做什么

这是 .mooncontainer 序列化的**主实现**。它把内存中的 MoonIR 模型（类型表、声明表、sysmeta 事实、导入导出、控制流图）拆成 Type/Manifest/Symbol/Contract/Imports/Exports/Sysmeta/Code 等节写入，或反向读出。

核心纪律（贯穿全文件注释）：

1. **边界即失败**：每个字符串/表/枚举/长度都受 ContainerLimits 约束，超界、非法 UTF-8、截断、枚举越界一律 reject，绝不修剪。
2. **原子发布**：decodeDeclarations/decodeCode/decodeContainer 先把整份解码进临时对象并过 Verifier，全部成功后才 move 到目标。任何中途失败都不会留下半成品 Module/Manifest。
3. **具体化（concrete projection）**：容器不携带尚未实例化的泛型模板/selector 等 recipe；encodeContainer/decodeContainer 都先构建具体化投影，且不携带 generic recipe。

- C++ 类比：一个带类型安全的小口径序列化库（类似 Google protobuf writer/reader），但把"合法性前置校验"与"失败时零副作用"作为纪律。

## 关键结构体·类

| 成员 | 含义 |
| --- | --- |
| anon::Encoder | 前向写字节：u32/u64/i64/boolean/enumeration/string/rows，全部先做 limit 与 UTF-8 检查。 |
| anon::Decoder | 前向读字节：u32/u64/boolean/enumeration/string/rowCount，全部带截断与范围防御。 |
| buildConcreteProjection | 把 Module 具体化为不含泛型 recipe 的投影。 |
| collectGraphReferences / collectFunctionReferences | 计算函数/图引用边，用于具体投影的传播。 |

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| codeOperationOpcode / codeExpressionOpcode | 把结构化 Stmt/Expr 映射到冻结 opcode（0 非法）。 |
| encodeTypes/decodeTypes ... 各 encodeXxx | 每节一个编/解码方法，内部用 Encoder/Decoder 的 rows/string 族。 |
| decodeDeclarations | 用 SymbolId 把 Symbol/Contract/Sysmeta 三节归一合并进 Module。 |
| decodeCode | 先解码所有函数行并入声明表，全部成功后才原子替换可执行声明。 |
| encodeContainer | 校验 Module、构建具体化投影、校验入口、逐节 append、交 ContainerWriter::encode 收尾。 |
| decodeContainer | parse 信封→分节解码→具体化投影→校验入口可执行→整份过 Verifier→才发布。 |
| decodeContainerForTarget | 额外绑定 expectedTargetTriple/dataLayout，不匹配即失败。 |

## 与周边文件·阶段的关系

- Container.h：提供外层 ContainerSection/Writer/Reader 信封与 ContainerLimits。
- ContainerModel.h：Interface 定义（含 encodeManifest 等 8 节契约）。
- Verifier.h：这个文件在多处调 Verifier::verify 作为发布门闸。
- 阶段：位于 Lowering、Sealer、Verifier 之后，是"最终固化/装载"的语义序列化层。

## 延伸阅读

- src/moonir/Container.h：字节信封。
- src/moonir/MoonIR.h：TypeRecord、DeclarationRecord、ControlFlowGraph 等被序列化的数据。
- src/moonir/Verifier.h：装载/固化前的模型校验。


---

---
title: Moon 容器模型分节编解码的接口定义
file: src/moonir/ContainerModel.h
namespace: moon
阶段: MoonIR 序列化 / Container 语义层
---

# src/moonir/ContainerModel.h

定义“容器内装的是什么”的语义模型：Manifest、8 个节的编码/解码契约，以及把整份 Module 与清单合成一份已验证容器的 ContainerModelCodec 静态接口。

## 这个文件做什么

Container.h 只管外层信封；这里管**信封里 8 个 section 各自装什么**。本文件以静态方法族的形式给出通往一个完整、可通过 Verifier 的 .mooncontainer 的窗口：

- 定义清单（Manifest）结构：包身份、目标三元组、入口点、特性开关等目标相关事实。
- 给出 8 个 required section（Type/Symbol/Contract/Imports/Exports/Sysmeta/Code，加 Manifest）各自的 encode/decode 契约。
- 提供把它们合成一项的 encode/decodeContainer，及给加载用的 decodeContainerForTarget。

与 Container.cpp 的“字节级信封”不同，本文件管的是“语义级 patch”。接口注释反复强调：解码完成前不发布任何部分状态（原子性），且整份模型必须先通过 Verifier 才能发布。

- 类比 C++ 读者：一个静态编解码工具集，类似 protobuf 的 Message 序列化入口，但刻意把验证前置。

## 关键结构体·类·枚举

| 名字 | 含义 |
| --- | --- |
| enum ContainerPackageKind | Application=1 / Library=2 包种类，影响入口（entrypoint）约束。 |
| enum CodeOperationOpcode | Code 节的“运算”标签：Let/Allocate/Expression/Free/Await（0 保留为非法）。 |
| enum CodeExpressionOpcode | Code 节表达式的标签全集：整数/浮点/字符串/调用/Move/Borrow 等 28 项。 |
| struct ContainerManifest | 清单：packageId、packageVersion、ContainerPackageKind、targetTriple、dataLayout、DeclarationRef entrypoint、FeatureFlags。 |
| class ContainerModelCodec | 全部 8 节的 encode/decode + container 合成/拆解；全静态。 |

辅助自由函数：codeOperationOpcode(const Stmt&)、codeExpressionOpcode(const Expr&)——把结构化 Stmt/Expr 标记到冻结的 opcode 枚举（0 保留为非法，防止被截断时误当作可执行）。

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| encodeManifest / decodeManifest | 编解码清单 Manifest（不序列化源文件路径）。 |
| encodeTypes / decodeTypes | 类型表；decode 会把非类型字段保留，仅替换类型表及索引。 |
| encodeSymbols/Contracts/Sysmeta | 声明表、契约与 sysmeta 事实分块编码。 |
| decodeDeclarations | 以 SymbolId 归一化联合三个节，失败不发布部分声明表。 |
| encodeImports/Exports / decodeInterfaces | 导入导出的接口层编解码并对齐。 |
| encodeCode / decodeCode | 可执行声明行；decode 在所有函数行成功加入后才原子替换。 |
| encodeContainer / decodeContainer | 8 节合成一个鉴权容器；decode 全程不发布直到通过 Verifier。 |
| decodeContainerForTarget | 额外绑定期望 targetTriple/dataLayout，不匹配不发布（供清单加载）。 |

## 与周边文件·阶段的关系

- Container.h域镜像：提供外层的 ContainerSection/Writer/Reader 与 ContainerLimits。
- ContainerModel.cpp：这些接口的具体字节编码实现。
- Verifier.h：encodeContainer/decodeContainer 前后用它整份校验 module，先验证再发布。
- 阶段：位于 Lowering、Sealer 之后，构成 .mooncontainer 持久化/载入的语义入口；decodeContainerForTarget 是运行时装载路径。

## 延伸阅读

- src/moonir/Container.cpp：8 节的字节（信封）层实现。
- src/moonir/MoonIR.h：被序列化的 Module/DeclarationRecord/ControlFlowGraph 定义。
- src/moonir/Verifier.h：模型完整性校验。


---

---
title: ControlFlowBuilder 实现：结构化到规范化 CFG 的展开
file: src/moonir/ControlFlowBuilder.cpp
namespace: moon
阶段: MoonIR 密封前的 CFG 构造
---
