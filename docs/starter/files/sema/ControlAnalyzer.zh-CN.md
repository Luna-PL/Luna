# src/sema/ControlAnalyzer.cpp — 槽位/片段/续延分析实现

> 一句话定位：`ControlAnalyzer` 的全部实现：slot 声明/调用、apply 绑定、fragment 契约核对与体分析、续延 once/many 控制路径检查。

## 这个文件做什么

把 `ControlAnalyzer.h` 的接口落地（约 500 行）。核心是：

1. 维护三层作用域栈：`mSlotScopes`（slot 声明）、`mApplyScopes`（静态 apply 绑定）、`mDynamicApplyScopes`（动态候选列表）。
2. `analyzeSlotInvoke` 时解析活动 slot 信息、约束参数、分析续延体，再解析 fragment 并做契约匹配。
3. `analyzeFragmentForSlot` 用「控制路径分析」（`ControlPaths` 结构 + 递归遍历）检查续延消费语义。

文件内定义局部结构 `ControlPaths { active(集合), aborted, returned, abortAfterResume }` 以及两个递归 lambda（`analyzePaths`/`analyzeStmtPaths`）。

## 关键结构体·类·枚举

- 文件内局部 `struct ControlPaths`：`std::set<int> active`（每个路径当前 resume 次数，封顶 2）、`aborted`/`returned`/`abortAfterResume` 标志。
- 使用 `ControlContextAccess::SlotInfo`（`SemanticContext.h`）。

## 关键函数·方法

- `analyzeSlotDecl`：去重 slot 名；解析参数类型与契约（`declaredType` + `parameterContractFor`）；填 `SlotInfo`；解析默认 fragment（`selectFragment` + 契约核对）；`Type::makeSlot` 结构类型；登记 `mSlotScopes.back()` 与符号表。
- `analyzeSlotInvoke`：
  - 三个分支决定「活动 slot」：隐式捕获（`isImplicitCapture`，就地建 slot）、内联接口（`interfaceParams`，解析参数并约束到局部绑定）、或查 `lookupSlot`。
  - 参数个数核对与逐参数 `constrain`。
  - 保存 `structuralType`/`resolvedParamNames`，`visibleSymbols()` 取捕获集，分析续延体。
  - fragment 解析三路：动态候选（`lookupDynamicApplied`，校验契约一致并逐个 `analyzeFragmentForSlot`）、静态 apply（`lookupApplied`）、默认片段；无绑定则视为 identity fragment（resume once）。
  - 校验 fragment 的 kind/cardinality 与 slot 契约一致。
- `analyzeApply`：
  - `selectFragment` 选主 fragment；与已知 slot 核对契约；动态 apply 时要求 slot 是 `dynamic slot`、候选须 `runtime`/`dynamic` retention、收集 `resolvedAlternativeFragmentNames`；有 body 则 `enterSlotScope` → 写 `mApplyScopes`/`mDynamicApplyScopes` → `analyzeBlock` → `exitSlotScope`。
- `analyzeFragmentForSlot`：
  - 参数个数核对；保存/恢复 `mCurrentFragmentSlot`/`mCurrentFragmentDecl`/`mCurrentReturnType`。
  - 进入作用域：绑定参数（`constrain` 到 slot 参数类型、核对 ownership 契约）、复制捕获符号。
  - `analyzeBlock(fragment->body, TyUnit)` 分析体。
  - 控制路径分析：对体做数据流式遍历（resume 使 active 计数 +1 封顶 2；abort/return 终止路径；if/match/while/for 合并路径），检查：single-shot context 不得 `abort()` 在 `resume()` 之后（`abortAfterResume`）、不得 resume 多次；重建 fragment 结构类型；many 片段禁止线性（linear）捕获（不可重放）。
- `enterSlotScope`/`exitSlotScope`：三栈同步压/弹（保底根层）。
- `selectFragment`：`mContext.mFragments` 查 `sourceDeclarationKey(name)`，未知报错。

## 与周边文件·阶段的关系

- `SemanticContext` 转发 `analyzeSlotDecl` 等入口到这里。
- 通过 `ControlContextAccess` 读写 `SemanticContext` 的 slot/apply/fragment 状态。
- `DeclarationCollector::declareFragment` 先登记 `mFragments`；`BodyAnalyzer` 遇到相关语句时转发；`TypeResolver` 回填 fragment 结构类型。
- 控制路径分析结果（`structuralType`/`resolved*Name`）被 MoonIR 生成使用。

## 延伸阅读

- `ControlAnalyzer.h`（接口）。
- `SemanticContext.h`（`SlotInfo` 与状态字段）。
- `DeclarationCollector.cpp`（fragment 登记）。
- `BodyAnalyzer.cpp`（调用入口）。


---

---
kind: source-file-guide
module: sema
source: src/sema/ControlAnalyzer.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 槽位/片段/续延系统的读者
---

# src/sema/ControlAnalyzer.h — 控制流分析器（ControlAnalysis 实现）

> 一句话定位：`ControlAnalyzer` 实现 `ControlAnalysis`：分析 `slot`（槽位）声明与调用、`apply`（片段绑定）与片段（fragment）对槽位的契合度，以及续延（continuation）的 once/many 语义。

## 这个文件做什么

Luna 有类「效应处理器」的控制结构：`slot` 是程序里可被替换的挂起点，`fragment` 是挂起点的实现（可绑定到一个或多个 slot），`apply` 在作用域内把 fragment 绑定给 slot，slot 调用（`resume()`/`abort()`）驱动续延。`ControlAnalyzer` 负责这些声明的语义分析：

- `analyzeSlotDecl`：登记 `slot` 声明（参数、契约、默认片段、种类/基数）。
- `analyzeSlotInvoke`：分析 slot 调用点（静态/动态、参数约束、续延体、fragment 解析与契约匹配）。
- `analyzeApply`：分析 `apply` 语句（fragment 绑定、动态候选、作用域）。
- `analyzeFragmentForSlot`：把 fragment 绑定到一个具体 slot 并分析其体（参数/契约核对、捕获、续延控制路径）。
- `enterSlotScope`/`exitSlotScope`：槽位/apply 作用域栈管理。
- `selectFragment`：按名查 fragment 并诊断未知名。

C++ 类比：类似「回调/挂钩（hook）机制」的类型检查：slot ≈ 事件点，fragment ≈ 事件处理器，apply ≈ 注册表。不过这里还叠加了续延（resume/abort）与单次/多次消费语义。

## 关键结构体·类·枚举

- `class ControlAnalyzer final : public ControlAnalysis`：唯一公开类型；私有成员 `ControlContextAccess mContext`。
- `ControlAnalysis` 接口（在 `SemanticContext.h`）：`analyzeSlotDecl`/`analyzeSlotInvoke`/`analyzeApply`/`analyzeFragmentForSlot`/`enterSlotScope`/`exitSlotScope`/`selectFragment`。
- 使用的 `SlotInfo` 定义在 `SemanticContext.h`（name/paramTypes/paramContracts/paramNames/defaultFragment/acceptedKind/acceptedCardinality/isImplicitCapture/isDynamic/structuralType）。

## 关键函数·方法

（语义见 .cpp 指南；这里列职责）

- `analyzeSlotDecl(SlotDeclStmt*)`：解析参数类型与契约、登记 `mSlotScopes`、构造 slot 结构类型、登记符号、解析默认 fragment。
- `analyzeSlotInvoke(SlotInvokeStmt*, expectedReturn)`：按静态/隐式捕获/内联接口三路解析活动 slot；参数约束；分析续延体；解析 fragment（静态 apply / 动态候选 / 默认）；校验契约并 `analyzeFragmentForSlot`。
- `analyzeApply(ApplyStmt*, expectedReturn)`：选 fragment、核对与已知 slot 的契约、处理动态 apply（多候选、runtime 要求、`mDynamicApplyScopes`）、进入 slot 作用域分析 `body`。
- `analyzeFragmentForSlot(FragmentDecl*, slotName, paramTypes, contracts, captures)`：核对参数个数与契约、进入作用域绑定参数与捕获、`analyzeBlock` fragment 体、然后做**控制路径分析**（`ControlPaths`：active/aborted/returned/abortAfterResume），检查 once/many 语义（single-shot 不得 resume 多次、不得 abort-after-resume 等），重建 fragment 结构类型，many 片段禁止线性捕获。
- `enterSlotScope`/`exitSlotScope`：同时压/弹 `mSlotScopes`/`mApplyScopes`/`mDynamicApplyScopes`。
- `selectFragment(name, useSite)`：`mContext.mFragments` 查找，失败报 `unknown fragment`。

## 与周边文件·阶段的关系

- 由 `SemanticContext` 的 `analyzeSlotDecl`/`analyzeSlotInvoke`/`analyzeApply`/`analyzeFragmentForSlot`/`enter/exitSlotScope`/`selectFragment` 转发调用。
- 通过 `ControlContextAccess` 访问 `mSlotScopes`/`mApplyScopes`/`mDynamicApplyScopes`/`mFragments`/`mCurrentFragmentSlot`/`mCurrentFragmentDecl` 等。
- `BodyAnalyzer` 在语句分析时遇到 `SlotDeclStmt`/`SlotInvokeStmt`/`ApplyStmt` 转到这里；`TypeResolver::materializeInferredTypes` 会回填 fragment 结构类型字段。
- 消费 `DeclarationCollector::declareFragment` 登记的 `mFragments`。

## 延伸阅读

- `ControlAnalyzer.cpp`（实现）。
- `SemanticContext.h`（`ControlAnalysis` 接口与 `SlotInfo`）。
- `BodyAnalyzer.cpp`（调用入口）。
- 语言特性背景：`docs/reference/` 中的 slot/fragment 设计。


---

---
kind: source-file-guide
module: sema
source: src/sema/DeclarationCollector.cpp
lang: zh-CN
audience: 学过 C/C++、想读 Luna 声明收集实现的读者
---
