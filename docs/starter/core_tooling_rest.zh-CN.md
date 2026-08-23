# Luna core / tooling / selector / instantiation / package / macro — 周边层与核心类型系统指南

Document category: implementation note
Applies to: Luna 0.3.0 development
Status: Implemented Experimental
Normative status: non-normative（本文只讲阅读与职责，不定义语言契约；语言规则以 docs/reference/ 为准）

> 由文档代理编写。文中每个符号都对照 src/ 当前源码核实后才引用，未虚构任何符号。
> 面向已学过 C++、熟悉 <memory>/<type_traits>、理解 struct 对齐 与 shared_ptr 的读者。

---

## 1. 定位：这些外围层如何服务主管线

Luna 的主流编译管线为：

    source / package
      -> MacroProcessor（宏展开）
      -> Lexer / Parser
      -> SemanticAnalyzer / TraitChecker / OwnershipChecker   (sema)
      -> 验证后 MoonIR
      -> MoonIR 优化 -> LLVM lowering -> ORC JIT / AOT

本文覆盖主线之外的支撑骨架：core（类型身份、所有权、大小/对齐、类型关系）、tooling（源码缓冲、代码导航索引、分析快照）、selector（编译期选择）、instantiation（泛型实例缓存）、package（包管理）、macro（宏管线）。它们不参与词法/语法/IR 计算，只提供类型系统数据、转发、缓存与工程边界。

职责总表：

| 目录/文件 | 职责 | 在主线中的位置 |
| --- | --- | --- |
| core/ | 类型对象 Type、身份、布局 | 全管线共享 |
| tooling/ | 源码缓冲、符号/引用索引、快照 | 前端输出之后，供 IDE/CLI |
| selector/ | 编译期选择（select/family） | 语义层 |
| instantiation/ | 泛型实例缓存与递归检测 | 语义层特化 |
| package/ | 文件/包 + 依赖合并成 Program | 管线入口打包 |
| macro/ | 每个源单元先过宏展开 | package 之后、lexer 之前 |
| main.cpp | 进程入口 | 调用 luna::driver::run |

一句话记忆：core 是“类型系统的数据结构 + 判定”，tooling 是“前端结果的只读快照”，其余是特性级骨架。

---

## 2. core 的职责

### 2.1 StableIdentity.h — 稳定强类型 ID

模板 StableId<Tag> 包装一个字符串 identity，为不同类型打上标签：TypeId、ShapeId、SymbolId、ContractId、AbiLayoutId；这些是不可互换的强类型。提供 FNV-1a 64 位哈希 stableIdentityHash，以及带前缀 + 16 位十六进制哈希的 compactIdentity（前缀如 symbol_/contract_/abi_），并有 symbolIdFromCanonical、contractIdFromCanonical、abiLayoutIdFromCanonical 便捷工厂；SymbolId 通常由包/模块/声明组合而成。

C++ 类比：类似 boost::strong_typedef，编译器拒绝把 SymbolId 当 TypeId 用；FNV-1a 得到确定性、跨进程一致的字符串 ID。

### 2.2 TypeIdentity.h

给出 TypeDomain（Value/Meta/Compiler/Inference/Error）与 IdentityMode（Structural/Nominal/Builtin/MetaSchema/CompilerIntrinsic/Inference/Error），并把 TypeId/ShapeId 收敛到 core。它回答“一个类型怎么判等”（结构 vs 名义 vs 内置）。

### 2.3 Ownership.h

Relation（Owned/SharedBorrow/MutableBorrow）、Usage（Copy/Affine/Linear）与 struct Contract{relation,usage}；附 inline 判定 isMoveOnly/mustConsume/usageStrength/strongerUsage，以及 CleanupAction（None/Drop/Deallocate/DeviceRelease/ResultDrop/EnumDrop/ArrayDrop/RecordDrop）。OwnershipChecker（sema）使用这些概念。

### 2.4 TypeSystem.h（最关键）

struct Type：kind、domain、identityMode、name、nominalId、typeParams/typeArgs、inner、arrayLength、isMutable、paramTypes/returnType、paramContracts/returnContract、capturedFields、fields（Struct/Record）、variants（Enum）以及 sysmeta（luna::sysmeta::Facts）。Type 以 std::shared_ptr<Type> 管理（TypePtr），带工厂 makePrimitive/makeStruct/makeEnum/makeRecord/makeResult/makeIterator/makeFunction/makeClosure/makeSlot/makeFragment/makeUnknown/makeInferenceVar，并预置 TyI32…TyUnknown 常量。

另有 defaultUsageForType/typeRequiresCleanup/cleanupActionForType/parameterContractForType/ResourceContract 等 inline 判定做所有权与清理推导。

### 2.5 SysMeta.h（编译器权威语义事实）

SchemaMajor/SchemaMinor 版本、Drop/From/Option/Iterator 等 Trait 与类型 ID、运行/支持 Flag（ControlForm/Cardinality/ContinuationStorage/Forwarding/ResourceManagement/ReleaseDomain/ResourceLifetime）合并成 Identity/Control/Resource/Capability/Abi 各 Facts，并组合为 IdentityFacts / ControlFacts / ResourceFacts / CapabilityFacts / AbiFacts，由 Facts 挂在 Type::sysmeta（编译器权威，源码不可覆盖）。

### 2.6 TypeRelations — 规范化与类型关系

把 Type 序列化成稳定 canonical 签名再判等：canonicalShape（结构签名）与 canonicalType（含名义子类）。关系判定 sameType/sameShape/isAssignable/isExplicitlyConvertible/isAbiCompatible/isRecursiveShape；实现使用 length-prefix 的 appendPart + FNV 哈希形成 compactId。canonicalShape→shapeId，canonicalType→typeId。

### 2.7 TypeLayout — sizeof / 布局

常量 InlineAdtAbiVersion=1、InlineTagStorageSize=8、InlinePayloadAlignment=8；函数 valueSize/valueAlignment/alignTo/productFieldOffset/variantFieldOffset/variantPayloadSize/enumPayloadSize/inlineAdtLayoutSignature。规则：Unit/Never=0、Slice=16、Array=len×elem、scalar 1/2/4/8；Result/Enum 用 8B 判别 tag + 对齐后的最大载荷。

---

## 3. tooling

### 3.1 SourceManager

管理一组 SourceDocument（id/text/version + 行起始偏移），提供 byteOffset(Utf16Position) 与 utf16Position（UTF-16 码元 ↔ 字节）并用 decodeUtf8 处理 1–4 字节码点；open/update/close/find，update 用 version 做乐观并发。

### 3.2 SymbolIndex

把 Program 的声明枚举成带 id/kind/name/签名/源位置的符号（区分 exported 与 external），symbolId 由 package+module+kind+linkage 拼成，可按 id/name/document 查找。

### 3.3 ReferenceIndex

把程序里的引用位置解析到目标 SymbolId（同位置去重、排序），为引用导航提供支持。

### 3.4 AnalysisSnapshot

对一次“前端就绪”结果做只读快照：持有 Program、semantic 上下文、符号/引用索引、包图与错误 stage；入口 analyzePath（读文件/包）、analyzeSource（纯内存）、analyzePathWithOverlay / analyzePathWithOverlays / analyzeSource 等，供 IDE 复用。

---

## 4. selector / instantiation / macro / package

### 4.1 selector（Selector.h/.cpp）

编译期选择（select/family）：候选视图 DeclarationView 与选择器 Engine。候选（Candidate）带 declarationId / symbolName / familyId / callableType / retention（CompileTime/Runtime/Dynamic），并带元数据；Engine::validate 校验“返回的恰好一个 id 来自该 view 且同 family”，结果型是 Result（ResultKind 为 Unique / NoMatch / Ambiguous / InvalidCandidate / InvalidView）；动态选择用 Engine::planDynamic 产出 DynamicPlan。

### 4.2 instantiation（Instantiator.h/.cpp）

泛型实例缓存：Request{genericDeclarationId, type/value/meta arg} → keyFor（FNV）；state New/InProgress/Ready/Failed，begin 保证唯一性并用 InProgress 做递归检测，complete/fail 落库，instanceIdFor 生成 __moon_inst_<hash> 命名。

### 4.3 macro（MacroProcessor.h/.cpp）

ExpansionLimits（maxDepth/maxGeneratedBytes/maxExpansions）与 process()：当前为占位实现，仅做字节上限检查并把原文+来源线索返回，尚未接真正的展开器。

### 4.4 package（PackageManager.h/.cpp + Package.h/.cpp）

从输入收集源（单文件/目录/workspace），解析 luna.package 及其依赖（registry 禁用，当前仅 workspace+lock），组装成合并的 Program 并给出 package graph；每个源单元 parseSource（macro→lexer→parser）。

---

## 5. main.cpp 入口

    #include "driver/Driver.h"
    int main(int argc, char* argv[]) { return luna::driver::run(argc, argv); }

入口共 5 行，把控制流交给 driver；driver 的 CompilerPipeline 统一：读包 → 词法/语法 → 语义 → （MoonIR 构建）→ 代码生成。

## 6. 阅读顺序 + 相关测试

建议顺序：StableIdentity → TypeSystem(Type) → TypeRelations → TypeLayout → Ownership/SysMeta → SourceManager → AnalysisSnapshot → Symbol/ReferenceIndex → selector → instantiation → macro → package，另对比 main + driver。

测试：tests/ 下有 core_contracts_test.cpp（StableIdentity/core 常量）、source_manager_test.cpp（SourceDocument 位置换算）、analysis_snapshot_test.cpp（AnalysisSnapshot + 引用索引），及大量 fixtures/*。

---
（完）