# src/sema/TraitChecker.cpp — trait 一致性检查实现

> 一句话定位：`TraitChecker` 的实现：注册 trait/impl 表，检查 `where` 子句，提供 `satisfies` 查询。

## 这个文件做什么

把 `TraitChecker.h` 的接口落地（约 130 行）。流程为：

1. `registerTraits`：内置 `Drop` 契约 + 遍历 `TraitDecl` 建立 `mTraitSigs`（trait id → 方法签名）。
2. `registerImpls`：遍历 `ImplDecl` 建立 `mImplMap`（traitId → typeId → methodName → 实现指针）。
3. `checkConcreteFunction`：核对每个函数的 `where T: Trait`。
4. `satisfies`：按已建表做查询。

文件内两个静态助手：`traitIdOf`（优先 `resolvedTraitId`，其次 `generatedSymbolName`/`name`）与 `typeIdOf`（`luna::types::typeId`）。

## 关键结构体·类·枚举

无新类型；使用头文件的 `MethodSig`、`mTraitSigs`、`mImplMap`。

## 关键函数·方法

- `check(Program*)`：`registerTraits` → `registerImpls` → 对每个 `FunctionDecl` 设诊断位置并 `checkConcreteFunction`；返回 `mErrors.empty()`。
- `registerTraits`：先把 `luna::sysmeta::DropTraitId` 的签名（`Drop::drop() -> unit`）写进 `mTraitSigs`；再对每个 `TraitDecl` 解析其方法参数/返回类型。
- `registerImpls`：遍历 `ImplDecl`；`traitId` 空则跳过（语义分析已诊断过）；按 `resolvedTargetTypeId`（为空用 `?`）把 `method->name → method` 写入 `mImplMap`。
- `checkConcreteFunction`：只处理 `WhereClause::Kind::TraitBound`；依次检查：trait id 已解析、类型参数确实在 `decl->typeParams` 里、trait 在 `mTraitSigs` 中存在；三类失败各有专属错误信息。
- `satisfies(type, traitName) const`：`mImplMap` 找 (traitName → typeId)；再比对 trait 方法签名集合是否被实现覆盖；无签名要求则直接为真。
- `error(msg)`：`diagnostic::format("trait", msg, file, line, col, hint)`。

## 与周边文件·阶段的关系

- 在 `SemanticContext` 之外独立运行；依赖语义分析已填写的 `resolvedTraitId`/`resolvedTargetTypeId`（`DeclarationCollector` 所写）。
- 类型工具来自 `src/core/TypeRelations.h`（`typeId`）与 `resolveType`（`TypeSystem.cpp`）。
- 属于「声明形状复核」类检查，与 OwnershipChecker 一样是 Sema 主流程之外的第二道防线。

## 延伸阅读

- `TraitChecker.h`（接口）、`DeclarationCollector.cpp`（trait/impl 注册）。
- `SemanticContext.cpp` 的 `satisfiesTrait`（运行时查询的主路径）。
- 语言特性背景：`docs/reference/` 中的 trait/约束设计。


---

---
kind: source-file-guide
module: sema
source: src/sema/TraitChecker.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna trait 校验的读者
---

# src/sema/TraitChecker.h — 独立于主流程的 trait 一致性检查器

> 一句话定位：`TraitChecker` 是 Sema 之外的第二道独立检查器：验证 `where T: Trait` 约束合法，并提供 `satisfies` 查询（某类型是否实现某 trait）。

## 这个文件做什么

主语义分析（`SemanticContext`）负责类型推断与符号绑定；trait 相关的「声明形状」检查则独立成一个小工具类 `TraitChecker`。它扫描整个 `Program`：

- 注册所有 trait 的方法签名（`registerTraits`）。
- 注册所有 impl 的方法映射（`registerImpls`）。
- 检查每个具体函数（`checkConcreteFunction`）的 `where` 子句：类型参数是否存在、trait 是否已知。
- 提供 `satisfies(type, traitName)` 供其他阶段查询。

C++ 类比：类似「概念（concepts）」层面的静态检查：trait ≈ 概念，impl ≈ 特化，`where T: Trait` ≈ 约束表达式。

注意它与 `SemanticContext::satisfiesTrait` 是两条路径：本类独立构建自己的表（`mTraitSigs`/`mImplMap`），用于独立复核。

## 关键结构体·类·枚举

- `struct MethodSig`：trait 方法签名摘要：`name`、`paramTypes`（`TypeVec`）、`returnType`。
- `mTraitSigs: unordered_map<string, vector<MethodSig>>`：trait（resolved id）→ 方法签名列表。
- `mImplMap: unordered_map<string, unordered_map<string, unordered_map<string, FunctionDecl*>>>`：traitId → (typeName → (methodName → `FunctionDecl*`))，与 `SemanticContext::mImpls` 同构。
- 诊断字段：`mErrors`、`mDiagnosticFile/Line/Col`。

## 关键函数·方法

- `check(Program*)`：入口：`registerTraits` → `registerImpls` → 遍历函数声明做 `checkConcreteFunction`；返回 `mErrors.empty()`。
- `registerTraits`：先内置 `Drop`（`DropTraitId`，`DropMethodName`，返回 `TyUnit`）；再遍历 `TraitDecl` 用 `resolveType` 解析方法签名存入 `mTraitSigs`。
- `registerImpls`：遍历 `ImplDecl`，按 `resolvedTraitId`/`resolvedTargetTypeId` 把方法指针写进 `mImplMap`。
- `checkConcreteFunction(decl)`：对每条 `where T: Trait` 子句检查：trait id 已解析、`T` 是声明的类型参数、trait 存在（否则报错）。
- `satisfies(type, traitName) const`：`mImplMap` 里查（type id, trait），再核对 trait 要求的每个方法名都实现；无方法要求直接 `true`。
- `error(msg)`：用 `diagnostic::format("trait", ...)` 收尾。

## 与周边文件·阶段的关系

- 与 `SemanticContext`/`SemanticAnalyzer` 并行存在：`TraitChecker::check` 在完整语义分析之外被驱动者调用（通常在其后做独立复核），持有自己的诊断列表。
- 使用 `src/core/TypeRelations.h` 的 `luna::types::typeId` 与 `resolveType`（`TypeSystem.cpp`）。
- 依赖 `ImplDecl`/`TraitDecl` 上已被语义分析填写的 `resolvedTraitId`/`resolvedTargetTypeId`（由 `DeclarationCollector` 填写）。

## 延伸阅读

- `TraitChecker.cpp`（实现）。
- `SemanticContext.cpp` 的 `satisfiesTrait`/`resolveTraitRef`（主流程内的 trait 逻辑）。
- `DeclarationCollector.cpp` 的 `declareTrait`/`declareImpl`（resolved id 的源头）。


---

---
kind: source-file-guide
module: sema
source: src/sema/TypeResolver.cpp
lang: zh-CN
audience: 学过 C/C++、想读 Luna 类型解析与特化实现的读者
---
