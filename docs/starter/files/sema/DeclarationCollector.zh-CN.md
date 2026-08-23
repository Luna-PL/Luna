# src/sema/DeclarationCollector.cpp — 声明收集实现

> 一句话定位：`DeclarationCollector` 的全部实现：登记函数/元数据/约束/片段/结构/枚举/特征/impl，校验元数据与 FFI 形状。

## 这个文件做什么

把 `DeclarationCollector.h` 的接口落地（约 590 行）。核心思想是：在分析任何函数体之前，先把「声明形状」登记为可查询状态（符号表、`mDeclaredTypes`、`mImpls`、`mTraits`、`mMetadataSchemas` 等），从而支持前向引用与声明顺序无关。

## 关键结构体·类·枚举

无新类型；使用 `DeclarationContextAccess` 与 `SemanticContext` 的字段。文件内局部 lambda：`declareImpl` 里的 `registerMethod`（登记单个 impl 方法的符号与契约）。

## 关键函数·方法

- `declareFunction`：
  - 用 `sourceKey = qualifiedDeclarationKey(package, module, name)` 组织 `mFunctionFamilies[sourceKey]`（同名重载族）。
  - 处理 where 子句（`resolveTraitRef` / 约束查找）；解析返回与参数类型（`declaredType` + typeParams bindings），计算 `usage`/`relation`（`parameterContractFor`/`defaultUsageForType`），写 `paramContracts`。
  - 首个同名函数 `defineAtRoot`；总写 `defineLinkage`；`isConstexpr` 登记 `mConstexprFunctions`；extern/导出带 ABI 时 `validateFFIFunction`。
- `declareMeta`：去重 schema；字段类型 `declaredType`；`Type::makeMetadata(identity, fields)` 注册 `mMetadataSchemas` 与符号表；构造 metadata 构造函数 `SymbolInfo`。
- `declareConstraint`：登记 `mConcepts[sourceKey]`，去重类型参数。
- `analyzeConstraint`：进入作用域绑定类型参数 → `analyzeExpr(predicate)` → 要求 bool/推断变量 → 退出作用域。
- `analyzeMeta`：逐字段 `checkUnresolved`。
- `validateMetadata(Decl*)`：对每个附件：查 schema（`sourceDeclarationKey`）、设 `resolvedSchemaId`、核对参数个数、`evaluateConstExpr` 求值每个参数并按值类型 `constrain` 到 schema 字段、推进 `retention`（Runtime/Dynamic 升级）。
- `declareFragment`：登记 `mFragments`；解析参数与契约；`Type::makeFragment` 结构类型；`defineAtRoot`。
- `isFFIType`：`resolved` 后按 `TypeKind` 白名单（整数/浮点/cstr/raw/unit/引用内层递归），否则报错。
- `validateFFIFunction`：ABI 只能 C；extern/export/constexpr/泛型互斥；linear 返回必须 `linear raw<T>`；逐参数与返回 `isFFIType`。
- `declareStruct`/`declareEnum`：取/建 `mDeclaredTypes[identity]`（预绑定于 `SemanticContext::analyze`），清空字段/变体后 `resolveTypeAST` 重新填充；登记符号表与类型表。
- `declareTrait`：拒绝 `Drop`/`From` 保留名；`traitIdentity` 设 `resolvedTraitId`；登记 `mTraits`/`mTraitTypeParams`/`mTraitOwners`；`Type::makeTrait` 注册。
- `declareImpl`：
  - 解析 `resolvedTraitId`/目标类型/owner；孤儿规则（`From`：target/source 至少一者在包内；`Drop`：必须拥有 target；普通 trait：trait 或 target 拥有）。
  - `From` 特判：单源类型参数、`mFromConversions` 登记、方法符号 `FromTraitId__<src>__for__<tgt>__<method>`。
  - `FromIterator` 特判：解析 item/builder/target，收集 begin/push/finish 进 `mFromIteratorImplementations`。
  - 常规：核对 trait 类型参数个数、`mImpls[traitId][targetId]` 去重、逐方法登记（`traitId__for__targetId__methodName`）。

## 与周边文件·阶段的关系

- 由 `SemanticContext::analyze` 的声明趟驱动（`.cpp` 各 declare/analyze 方法）。
- 消费 `DeclarationContextAccess`（`mContext`）读写 `SemanticContext` 状态。
- 产出供 `TypeResolver`（类型解析/特化）、`BodyAnalyzer`（impl 体校验、Drop 契约）、`ControlAnalyzer`（片段登记）使用。
- `resolveTraitRef`/`typeIdentity` 等经由 access 转发到 `SemanticContext`。

## 延伸阅读

- `DeclarationCollector.h`（接口）。
- `SemanticContext.cpp`（多趟调度）。
- `SemanticAnalysisSupport.h`（`nominalDeclarationIdentity`/`effectivePackageId`/`nominalTypeOwner`）。
- `BodyAnalyzer.cpp`（`analyzeImpl` 消费 impl 表做方法签名校验）。


---

---
kind: source-file-guide
module: sema
source: src/sema/DeclarationCollector.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 声明收集的读者
---

# src/sema/DeclarationCollector.h — 声明收集器（DeclarationAnalysis 实现）

> 一句话定位：`DeclarationCollector` 实现 `DeclarationAnalysis`：遍历每个声明并把「名字 → 符号/类型」登记进 `SemanticContext`，同时校验 FFI、元数据、约束等声明形状。

## 这个文件做什么

语义分析的第一趟是「声明收集」：在检查任何函数体之前，先把所有声明的名字、签名、类型、契约登记进符号表与注册表，保证前向引用成立。`DeclarationCollector` 就是这趟的组件：

- `declareFunction/Meta/Constraint/Struct/Enum/Trait/Impl/Fragment`：各声明类型的登记。
- `analyzeConstraint/analyzeMeta`：声明后的小检查（谓词必须 bool、元数据字段无未解析类型）。
- `validateMetadata(Decl*)`：给任意声明校验其元数据附件（schema 存在、参数个数、编译期值）。
- FFI 检查：`isFFIType`/`validateFFIFunction`。

C++ 类比：相当于 C++ 编译里「先扫描全部声明建立作用域/类型注册」的阶段（类似对每个 namespace/class 先做声明再定义）。

## 关键结构体·类·枚举

- `class DeclarationCollector final : public DeclarationAnalysis`：唯一公开类型；私有成员 `DeclarationContextAccess mContext`。
- `DeclarationAnalysis` 接口（在 `SemanticContext.h`）：`declareFunction`/`declareMeta`/`declareConstraint`/`analyzeConstraint`/`analyzeMeta`/`validateMetadata`/`declareFragment`/`isFFIType`/`validateFFIFunction`/`declareStruct`/`declareEnum`/`declareTrait`/`declareImpl`。

## 关键函数·方法

（语义见 .cpp 指南；这里列职责）

- `declareFunction`：解析参数/返回类型（`declaredType` + bindings）、计算所有权契约（`parameterContractFor`/`defaultUsageForType`）、写 `mFunctionFamilies`/`mSymTable.defineAtRoot`/`defineLinkage`、登记 `constexpr` 函数、必要时 `validateFFIFunction`。
- `declareMeta`：登记元数据 schema（`mMetadataSchemas`），构造 metadata 类型与「构造函数」符号。
- `declareConstraint`：登记约束名与去重类型参数。
- `analyzeConstraint`：在局部作用域绑定类型参数后分析谓词，要求其类型为 bool/推断变量。
- `analyzeMeta`：检查字段 `inferredType` 无未解析。
- `validateMetadata`：校验附件 schema/参数个数，`evaluateConstExpr` 求值并 `constrain` 到 schema 字段类型，处理 retention 升级。
- `declareFragment`：登记片段，构造 `Type::makeFragment` 结构类型与所有权契约。
- `isFFIType`：递归判定 C ABI 可表示类型（标量、cstr、raw、引用等），否则报错。
- `validateFFIFunction`：校验 ABI 为 C、extern 不与 export/constexpr/泛型混用、`linear raw<T>` 返回等。
- `declareStruct`/`declareEnum`：把字段/变体解析成 `TypeField`/`TypeVariant` 填进 `mDeclaredTypes` 的已预绑定类型（用 `resolveTypeAST` + bindings）。
- `declareTrait`：拒绝保留名（Drop/From）、登记 `mTraits`/`mTraitTypeParams`/`mTraitOwners`、注册 trait 类型。
- `declareImpl`：解析 trait 引用与目标类型，登记 `mImpls`；特判 `From`（一对一转换表）、`Drop`（孤儿规则）、`FromIterator`（builder 协议表）；`registerMethod` lambda 统一登记方法符号。

## 与周边文件·阶段的关系

- `SemanticContext::analyze` 在声明趟调用它（declareMeta/Constraint/Struct/Enum/Trait/Function/Fragment/Impl）。
- 通过 `DeclarationContextAccess` 访问 `SemanticContext`（`mSymTable`/`mDeclaredTypes`/`mImpls`/`mMetadataSchemas` 等）。
- 其登记结果（`mDeclaredTypes`/`mImpls`/`mTraits`/符号表）被 `TypeResolver`/`BodyAnalyzer`/`ControlAnalyzer` 消费。
- 用 `SemanticAnalysisSupport.h` 的 `nominalDeclarationIdentity`/`qualifiedDeclarationKey`/`effectivePackageId` 等工具。

## 延伸阅读

- `DeclarationCollector.cpp`（实现）。
- `SemanticContext.h`（`DeclarationAnalysis` 接口与注册表字段）。
- `SemanticAnalysisSupport.h`（身份/键工具）。
- `BodyAnalyzer.cpp`（消费 impls/traits 做体检查）。


---

---
kind: source-file-guide
module: sema
source: src/sema/Inference.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 类型推断的读者
---
