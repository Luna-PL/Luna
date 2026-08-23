# src/sema/SemanticContextAccess.cpp — 访问面工厂与转发的实现

> 一句话定位：实现五个 `xxxAccess()` 工厂（构造 `*ContextAccess` 并把上下文字段绑定到引用）与所有转发方法（`mOwner.xxx(...)`）。

## 这个文件做什么

纯「接线」文件：为五个 access 类写构造函数（初始化列表把 `SemanticContext` 字段绑定为成员引用）与转发方法。没有分析逻辑。

## 关键结构体·类·枚举

无新增类型。

## 关键函数·方法

- `SemanticContext::bodyAccess()` 等五个工厂：`return BodyContextAccess(*this);` 等。
- 每个 access 构造函数：例如 `BodyContextAccess(SemanticContext& context) : mOwner(context), mConcepts(context.mConcepts), ...`（每个字段一行）。
- 转发方法示例：`BodyContextAccess::constrain` → `mOwner.constrain(actual, expected, context)`；`analyzeApply` → `mOwner.analyzeApply(stmt, std::move(expectedReturn))`；`error` → `mOwner.error(...)`；`lookupSymbol` → `mOwner.lookupSymbol(name)` 等。
- 各 access 覆盖其声明的方法全集（见 .h）。

## 与周边文件·阶段的关系

- 被 `SemanticAnalyzer.cpp` 装配流程使用。
- 组件（`BodyAnalyzer` 等）只通过 access 读上下文，保证字段可见性最小化。
- 头文件 `SemanticContextAccess.h` 声明；本文件实现。

## 延伸阅读

- `SemanticContextAccess.h`（声明与字段清单）。
- `SemanticContext.h`（被引用成员）。


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticContextAccess.h
lang: zh-CN
audience: 学过 C/C++、想理解 Sema 组件拆分机制的读者
---

# src/sema/SemanticContextAccess.h — 组件级「访问面」封装

> 一句话定位：定义五个 `*ContextAccess` 类（`BodyContextAccess`/`DeclarationContextAccess`/`ControlContextAccess`/`TypeContextAccess`/`CompileTimeContextAccess`），把 `SemanticContext` 的字段与方法按组件暴露出去，实现「字段级依赖注入」。

## 这个文件做什么

语义分析被拆成五个组件，但它们的共享状态都活在 `SemanticContext` 里。这个头文件定义每个组件拿到的「访问卡」：

- 每个 `*ContextAccess` 是 `SemanticContext` 的 friend，持有 `SemanticContext& mOwner`。
- 通过 `decltype(SemanticContext::mXxx)&` 成员引用，把该组件需要的字段**别名绑定**到上下文对应字段（引用语义，改动直接作用到上下文）。
- 通过转发方法把上下文上需要的方法暴露给组件。

这样做的目的（头文件注释）：保持组件算法原有字段名不变，同时把「新增跨组件依赖」变成头文件改动——组件不再需要访问整个上下文。

C++ 类比：像「作用域化的 friend + 引用聚合」：每个组件拿到一个只含自己需要的字段/方法的视图，类似接口隔离（Interface Segregation）的字段版本。

## 关键结构体·类·枚举

- `class BodyContextAccess final`：体分析组件视图。
  - 字段：`mConcepts`、`mConstexprFunctions`、`mConstraints`、`mCurrentFragmentDecl`、`mCurrentFunctionReturnUsage`/`mCurrentFunctionReturnsLinear`、`mCurrentModulePath`/`mCurrentPackageId`、`mCurrentReturnType`、`mDeclaredTypes`、`mFromConversions`、`mFromIteratorImplementations`、`mFunctionFamilies`、`mGeneratedInstances`、`mImpls`、`mInFunction`/`mInKernel`、`mInferenceRoots`、`mIteratorStateCounter`、`mMetadataSchemas`、`mProgram`、`mSawReturn`、`mSymTable`、`mTraitMethods`、`mTraits`。
  - 方法：`analyzeApply`/`analyzeSlotDecl`/`analyzeSlotInvoke`、反射调用、`constrain`/`declareFunction`/`declaredType`/const 作用域、`error`、const/constraint/selector 求值、`instantiateNominal`/`lookupSymbol`/`monomorphize`、引用记录、`requireBool/Numeric/Integer`、`resolveTypeAST`/`resolved`/`satisfiesTrait`/`setDeclarationContext`/`setDiagnosticLocation`/`sourceDeclarationKey`/`traitIdentity`/`typeIdentity`/`typeToAST`。
- `class DeclarationContextAccess final`：声明收集组件视图（字段含 `mFragments`、`mTraitOwners`、`mTraitTypeParams` 等；方法含 `analyzeExpr`/`checkUnresolved`/`resolveTraitRef` 等）。
- `class ControlContextAccess final`：控制流组件视图（`mApplyScopes`、`mCurrentFragmentSlot`、`mDynamicApplyScopes`、`mSlotScopes` 等；`analyzeBlock`/`analyzeExpr` 等）。
- `class TypeContextAccess final`：类型组件视图（`mConstraints`、`mInstantiator`、`mInstantiatedFunctions`、`mQualifiedDeclarations`、诊断字段等；`lookupDeclaredType` 等）。
- `class CompileTimeContextAccess final`：编译期求值组件视图（`mActiveSelectorView`、`mConstEvaluationDepth`、`mConstScopes` 等；`analyzeExpr`/`resolveTypeAST` 等）。
- 各 access 均 `friend class SemanticContext`，构造函数私有、由 `SemanticContext` 的 `xxxAccess()` 工厂创建。

## 关键函数·方法

- `SemanticContext::bodyAccess()` 等五个工厂方法（在 .cpp 实现），返回对应 access 值对象。
- 每个 access 的构造函数：把需要的上下文字段逐个绑定到成员引用（初始化列表）。
- 每个转发方法：`return mOwner.xxx(...)`（例如 `BodyContextAccess::constrain` → `mOwner.constrain`）。

## 与周边文件·阶段的关系

- `SemanticAnalyzer.cpp` 构造组件时传 `mContext->bodyAccess()` 等；再 `bindXxxAnalysis` 回绑。
- 组件实现（`BodyAnalyzer` 等）通过 access 的字段引用读写 `SemanticContext` 状态。
- `SemanticContextAccess.cpp` 实现工厂方法与转发。
- 这层封装让五组件互不直接依赖，只依赖 `SemanticContext` 的接口面。

## 延伸阅读

- `SemanticContextAccess.cpp`（实现）。
- `SemanticContext.h`（被引用的字段与接口）。
- 各组件 .h/.cpp（消费者）。


---

---
kind: source-file-guide
module: sema
source: src/sema/SymbolTable.cpp
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 编译器前端的读者
---
