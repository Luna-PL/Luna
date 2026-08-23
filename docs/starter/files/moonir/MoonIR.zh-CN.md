# src/moonir/MoonIR.cpp

实现 MoonIR.h 里最难的部分：类型注册与密封（identity/布局规范化）、模块索引重建、各 find 查询，以及 TypeMaterializer 递归重建 Type 图。

## 这个文件做什么

除了各 Name() 字符串映射与 isCompilerIntrinsicName（MoonIR.h 定义可见），本文件还承担：

- **registerType**：把前端 Type 对象冻结成 TypeRecord，算规范类型/shape/ABI 布局字符串与 identity hash，去重、处理前向占位与完成的同 ID 合并，并递归注册引用的子类型。
- **sealTypeTable**：排序类型表、用 TypeMaterializer 反重建每个记录，做校验（TypeId 不得漂移）、重算 shape/布局/资源契约并重写 contract/identity。
- **rebuildIndexes**：由数据序列重新建立声明/类型 lookup map——这是"重建即更新"的边界，反序列化模块不得携带过期前端指针。
- **TypeMaterializer::materialize**：由常规记录递归重建 Type 图。

- 类比 C++ 读者：类似一个深拷贝/定型工具：先登记（pointer→record），再"封印"做一致性固化，后端用物化器还原成对象图。

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| Module::registerType(const TypePtr&) | 冻结为 TypeRecord；去掉重合并；递归注册子类型；表已 sealed 则抛 logic_error。 |
| Module::sealTypeTable() | 排序+物化验算+重算 identity/layout/contract；TypeId 漂移或 conflicting payload 抛 logic_error。 |
| Module::rebuildIndexes() | 重建 types/declarationRecords/functions/fragments 等 lookup 索引。 |
| Module::findType/findDeclaration(by symbol/ref/byId/byLinkage) | 多种按身份查声明/类型，找不到返回 nullptr。 |
| TypeMaterializer::materialize(TypeRef) | 递归重建 Type（记忆化），骨架先发以支持递归 nominal。 |
| canonicalAbiLayout/canonicalContract | 生成 luna.abi-layout.v1 / luna.contract.v1 前缀的稳定字符串。 |
| isCompilerIntrinsicName | 判断是否为编译器内建名（print、panic、type_of、is_ok 等长列表）。 |

## 与周边文件·阶段的关系

- 上层：Lowering 调用它产生/填充 Module 与 Type。
- 下游：ControlFlowBuilder 要求 typeTable 已 sealed；ContainerModel 序列化；Printer 打印；Verifier 校验。
- 阶段：构造中期的关键——类型表注册完成即 sealed，之后的 CFG 构造与容器化都以冻结表为准。

## 延伸阅读

- src/moonir/MoonIR.h：数据结构定义。
- src/moonir/Lowering.cpp：把前端 AST 降级为 Module。
- src/moonir/Verifier.h：对密封后的冻结表做完整性校验。
- src/core/TypeSystem.h：类型身份/布局的底层能力。


---

---
title: MoonIR —— Luna 中间表示的核心定义
file: src/moonir/MoonIR.h
namespace: moon
阶段: 前端 Lowering 产物 / 后端与运行时输入
---

# src/moonir/MoonIR.h

Luna 编译器的核心中间表示：定义"冻结的、无指针的、可序列化"的 Module、类型表、声明表、控制流图及其全部节点类型。

## 这个文件做什么

这是 MoonIR 的"宪法"。它定义一切可执行参考是"稳定表引用"（SymbolId/ContractId/TableRef），而非前端对象指针；定义类型/声明的记录结构；定义结构化构造输入（body）与规范化 CFG（controlFlow）的二分法。几乎其它所有 moonir 组件（Lowering/ControlFlowBuilder/Verifier/ContainerModel/Printer/Sealer/Optimizer）都以它为类型基础。

几个贯穿的设计原则：

- **稳定引用优于指针**：TypeRef=TypeId、SymbolRef=SymbolId、BlockId/RegionId/ScopeId/LocalId/CleanupId=TableRef。空引用=缺失描述，绝不指向"未解析对象"。
- **冻结无指针**：TypeRecord 是 pointer-free 的完整叙述，附带 referencedTypeIds 遍历索引，独立后端无需前端 Type 指针即可重建类型图。
- **两种体不得共存**：FunctionDecl 的构造期 body 与密封后 controlFlow 二选一（Sealer 负责切换）。
- **非 SSA、面向局部**：CFG 基于 LocalId 表而非 SSA 值编号，表索引即序列化身份。

## 关键结构体·类·枚举（精要）

| 名称 | 含义 |
| --- | --- |
| TableRef<Tag> 及 Block/Region/Scope/Local/Cleanup Id | 泛化稳定 ID，基于 uint32 索引，InvalidTableIndex 表示空。 |
| struct DeclarationRef | symbol + contract 双重别名，可执行引用的最小身份。 |
| struct TypeRecord | 完整类型载荷：kind/domain/identity、fields/params/return、abiLayout、sysmeta、referencedTypeIds。 |
| struct DeclarationRecord | 声明的全部关键信息：类型引用、contract、sysmeta、dropGlue 等。 |
| struct ControlFlowGraph | 区块/区域/作用域/局部/清理五张表 + sealed 标志与 find* 查询。 |
| struct Terminator/BasicBlock | 终止器（Jump/Branch/Switch/Return/Resume/Abort…）与基本块。 |
| struct Module | 顶层：类型/声明/导入/导出/Costs，外加索引 map 与注册/密封/查找方法。 |
| class TypeMaterializer | 由常规记录重建 Type 图，缓存外部于 Module。 |

枚举族：RegionKind、LocalKind、CleanupKind、TerminatorKind、ProjectionKind、DeclarationKind、FragmentKind/Cardinality、Operator、CostKind、ImportKind、Retention、ContinuationKind。

## 关键函数·方法

| 函数/方法 | 作用 |
| --- | --- |
| Module::registerType / sealTypeTable | 注册类型（去重、保结构、碰撞即 error）；密封后排序并规范化 identity/layout。 |
| Module::rebuildIndexes / findDecl/findType/findDeclaration* | 重建查找表；多路 find 按 id/symbol/contract/linkage。 |
| ControlFlowGraph::findBlock/findRegion/findScope/findLocal/findCleanup | 按表引用取对应记录。 |
| TypeMaterializer::materialize | 由常规记录、递归重建 Type 图（骨架先发以支持递归 nominal）。 |
| canonicalAbiLayout / canonicalContract | 生成稳定的规范字符串（Layout/Contract），用于 identity hash。 |
| isCompilerIntrinsicName | 判断名字是否是无声明表行的编译器内建名。 |
| *Name(枚举) 一组 | 把各枚举映射为稳定字符串（调试/序列化用）。 |

## 与周边文件·阶段的关系

- 是 moonir 各组件（Lowering、ControlFlowBuilder、Verifier、Printer、Sealer、Optimizer、ContainerModel、Container）的类型基础。
- 阶段：前端 Lowering 的产出；经 CFG 构造、Verifier 验证，最终通过 ContainerModel 持久化。

## 延伸阅读

- 具体实现：src/moonir/MoonIR.cpp（canonicalAbiLayout、registerType、find*、TypeMaterializer）。
- src/moonir/Lowering.h/.cpp：生成本 IR 的入口。
- src/moonir/ControlFlowBuilder.h：从结构化 body 构造第一条 CFG。
- src/moonir/ContainerModel.h：把这里的数据序列化成 8 个节。
- ContainerModel.h：把这里的数据序列化成 8 个节。

---

---
title: Optimizer 实现（当前为 canonicalization）
file: src/moonir/Optimizer.cpp
namespace: moon
阶段: MoonIR 后处理
---
