# Luna 语义分析（Sema 子系统）阅读指南

> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative（本文档只讲解 Sema 如何读代码，不定义语言契约；语言规范以 `docs/reference/` 为准）

本指南面向学过 C/C++ 但不了解编译原理的读者。你不需要知道 SSA、CFG、LLVM IR，也不需要会写类型推导。每条概念都会先用 C/C++ 类比讲清楚，再落到 `src/sema/` 目录的真实代码符号。建议边读边对照源码文件。

Luna 的编译主线（Sema 位于 Parser 之后、MoonIR 验证之前）：

```text
source/package
  -> Lexer / Parser
  -> SemanticAnalyzer / TraitChecker / OwnershipChecker   # Sema，本文主角
  -> verified MoonIR -> optimizer -> LLVM -> ORC JIT / AOT linker
```

---
## 2. 一句话定位

语义分析（Sema）把 Parser 产出的抽象语法树（`Program` / `Decl` / `Stmt` / `Expr`）就地注解成「类型完备、符号已绑定、所有权已核实」的**同一棵树**，并把后端做 MoonIR 所需的工程信息写回 AST。它位于 Parser 之后、MoonIR 验证之前，是前端最后一步。

Sema 消费一棵「每个名字都还是文本」的 AST：给 `Expr` 填 `resultType`，给调用填 `resolvedSymbolName`，给绑定填 `inferredType`，给声明填 `generatedSymbolName`。后端拿到被填满的 AST 直接生成 MIR，**永远不会重做名字解析或类型推断**。

---
## 3. 语义分析职责

1. **符号绑定**：把自由名字（`foo`、`Point`、`Result`）解析成具体声明，并记录引用位置供 IDE 跳转（`ResolvedDeclarationReference`）。见 `SymbolTable`、`SemanticContext::sourceDeclarationKey` / `lookupSymbol`。
2. **类型检查与推断**：为表达式推导或约束出具体 `Type`，校验整数/浮点/布尔/比较运算符。见 `TypeResolver`、`ConstraintSolver`。
3. **所有权与借用检查**：仿 Rust 但更朴素，跟踪每个变量是 `Owned / SharedBorrow / MutableBorrow`、是否已被 `Moved/Freed`，保证线性值被恰好消费、堆值在作用域出口释放。见 `OwnershipChecker`。
4. **trait / impl / 约束**：trait 方法签名匹配、impl 孤儿规则（orphan rule）、`where` 子句。见 `TraitChecker` 与 `SemanticContext` 的 `mImpls`。
5. **编译期求值**：`const`、`constexpr fn`、类型反射（`type_of` 等）、元数据、约束谓词、声明选择器。见 `CompileTimeEvaluator`。
6. **控制流与 continuation**：slot / fragment / resume / abort 的一次（once）或多次（many）语义与跨边界所有权。见 `ControlAnalyzer`。

关键工程决策：Sema 拆成多个小分析器，每个只为单一职责服务，但**共享同一个权威中央状态 `SemanticContext`**。

---
## 4. 核心数据结构（用 C++ 类比）

### 4.1 TypePtr —— 类型不是整数句柄，而是 shared_ptr

前端的「类型」就是 `std::shared_ptr<Type>`，别名写作 `TypePtr`（见 `src/core/TypeSystem.h`）。`struct Type` 只有数据没有虚函数：`TypeKind kind`、`std::string name`、`nominalId`、`typeArgs`、`paramTypes`、`fields` 等。

> 澄清：源码里并没有名为 `SymbolId` 或 `TypeId` 的整数句柄类型；身份由「名字字符串 + 共享 Type 指针」表达。

### 4.2 类型身份：nominalId + luna::types::typeId()

结构身份：`Record`（匿名 record）按字段名与字段类型相等。名义身份：具名 `struct` / `enum` / `trait` 靠 `nominalId` 区分，两字段完全相同的具名 struct 仍不相等。把 `Type` 转成稳定字符串身份用 `SemanticContext::typeIdentity(type)` -> 返回 `luna::types::typeId(type).value`。

### 4.3 SymbolInfo 与 SymbolTable

```cpp
enum class SymbolKind { Variable, Function, Fragment, Slot, Struct, Trait, TypeParam, Metadata };
struct SymbolInfo { SymbolKind kind; TypePtr type; TypeVec paramTypes; TypePtr returnType;
    std::vector<std::string> typeParams; bool isHeapAllocated; bool isLinear;
    luna::ownership::Usage usage;  Relation relation;
    bool returnsLinear;  Usage returnUsage;  FunctionDecl* genericDecl; }
```

`SymbolTable` 存一对作用域栈：`std::vector<unordered_map<name, SymbolInfo>> mScopes` + `mTypeMap` + `mLinkageSymbols`。常用：`enterScope` / `exitScope`、`define` / `defineAtRoot` / `defineLinkage`、`lookup`、`lookupDepth`（闭包捕获分析用）。命名隐藏规则与 C/C++ 一致。

> 后端**不读** `SymbolTable`：类型与符号在分析后已被写回 AST（如 `LetStmt::inferredType`），`SymbolTable` 只有前端生命周期。

### 4.4 SemanticAnalyzer 与 SemanticContext

`src/sema/SemanticAnalyzer.h`（约 49 行）是 Sema 唯一外部入口，构造函数把六个子分析器装配并把五个接口 bind 到 `SemanticContext`：

```cpp
mBodyAnalyzer(make_unique<BodyAnalyzer>(mContext->bodyAccess()));
mTypeResolver(make_unique<TypeResolver>(mContext->typeAccess()));
mCompileTimeEvaluator(make_unique<CompileTimeEvaluator>(mContext->compileTimeAccess()));
mDeclarationCollector(make_unique<DeclarationCollector>(mContext->declarationAccess()));
mControlAnalyzer(make_unique<ControlAnalyzer>(mContext->controlAccess()));
```

`SemanticContext`（约 452 行，`src/sema/SemanticContext.h`）才是 Sema 的心脏：`mSymTable`、`mMetadataSchemas`、`mConcepts`、`mFunctionFamilies`、`mQualifiedDeclarations`、`mPackageAliases`、`mImpls`、`mFromConversions`、`mFromIteratorImplementations`、`mTraitTypeParams / mTraitMethods / mTraitOwners`、`mFragments`、`mDeclaredTypes`；还有 `ConstraintSolver mConstraints`、`mInferenceRoots`、slot 状态 `mSlotScopes` 等。

它公布五个纯虚接口（`BodyAnalysis`、`TypeAnalysis`、`CompileTimeAnalysis`、`DeclarationAnalysis`、`ControlAnalysis`，都在 `SemanticContext.h`），`analyze()` 里所有 pass 都通过这些接口调用真正实现。

### 4.5 SemanticContextAccess 与 Capability

`SemanticContextAccess.h`（252 行）定义 5 个能力对象：`Body/CompileTime/Control/Declaration/TypeContextAccess`。每个对象持有对 `SemanticContext` 成员的一组合法引用，构造函数 private 且只被 `friend SemanticContext` 构造，从而只暴露「本分析需要」的状态/动作。C++ 类比：friend + 窄接口包装。

---
## 5. 各 .cpp 职责表

| 文件 | 接口 | 职责 | 关键方法 |
|---|---|---|---|
| SemanticAnalyzer.cpp | 门面 | 装配并转发 | analyze / errors / symTable |
| SemanticContext.cpp | 中央 | pass 编排、索引 | analyze / typeIdentity / error |
| SymbolTable.cpp | 符号表 | 作用域栈 | enterScope / lookupDepth |
| TypeResolver.cpp | TypeAnalysis | 类型/约束/实例化 | resolveTypeAST / declaredType / monomorphize / constrain |
| DeclarationCollector.cpp | DeclarationAnalysis | 声明形状 | declare* 系列 / isFFI |
| BodyAnalyzer.cpp | BodyAnalysis | 函数体/闭包/起始器 | analyzeFunction / analyzeStmt / analyzeExpr |
| ControlAnalyzer.cpp | ControlAnalysis | slot/fragment | analyzeSlotDecl / Invoke / Apply |
| CompileTimeEvaluator.cpp | CompileTimeAnalysis | 反射/const/selector | analyzeReflectionCall / evaluateConst |
| OwnershipChecker.cpp | Ownership | 所有权/借用 | consume / acquireLoan / validateLinear |
| TraitChecker.cpp | Trait | trait 校验 | check / registerImpls |
| Inference.h + TypeSystem.cpp | ConstraintSolver | 推断 | fresh / unified / resolve |

---
## 6. TypeResolver 做什么

- `resolveType()`（初版，`src/sema/TypeSystem.cpp`）：只处理字面类型（i32/array/raw/device_buffer…）并按名字构造空 struct。
- `resolveTypeAST()`（TypeAnalysis 实现）：先查 `mSymTable` 类型参数，再匹配内建关键字，最后到 `mDeclaredTypes` 查用户声明并做 `instantiateNominal`（展开泛型）。
- `declaredType(auto)` -> 返回 `mConstraints.fresh()`（推断变量）。
- `resolved()`：解开推断变量并刷新 Drop 资源事实（防缓存过期）。
- `monomorphize()`：克隆泛型函数体、替换参数，经 `luna::instantiation::Instantiator` 缓存，返回唯一实例。

---
## 7. 类型推断（ConstraintSolver）

`fresh()` 新建推断变量；`resolve()` 展开；`unify()` 求解（`unifyInternal`）；`requireNumeric/Bool` 记录约束；`defaultUnconstrainedNumeric()` 数值默认 i32；`hasUnresolved`。私有 `mBindings`、`mNumeric/BoolConstraints`、`contains`（occurs-check 防递归）。`Reference` 把 `isMutable` 当类型一部分（`&mut T` 与 `&T` 不等）。

---
## 8. OwnershipChecker（所有权/借用）

`src/core/Ownership.h` 定义两个正交维度：`Relation {Owned, SharedBorrow, MutableBorrow}`（谁管）与 `Usage { Copy, Affine, Linear }`（能用几次）；`Contract{Relation,Usage}` 组合两者；`isMoveOnly=Affine||Linear`、`mustConsume=Linear`。

类型的默认 usage 由 `src/core/TypeSystem.h` 的 `defaultUsageForType` 递归计算；`typeRequiresCleanup` / `cleanupActionForType` 决定出作用域清理。状态机：`OwnState{Valid, Moved, Freed}`、`Place{root,components}`（存储位置）、`Loan{source,isMutable}`；`placesOverlap` 做前缀重叠 → 部分 move / 借用冲突依据。

关键：`checkFunction` 逐参数 define、`validateLinearScope`、`collectFreesAtScopeExit` 生成隐式 `FreeStmt{isImplicit=true}`、填 `Return` 的 `cleanups`。`consume` 把 Affine/Linear 标 `Moved`；`acquireLoan` 管借用冲突；循环不能改外层变量所有权。产物：作用域出口的隐式 `FreeStmt`；return/abort 处的 `CleanupObligation{place, action, type}`。

### C++ 类比
- 存储所有权/借用 ~ `unique_ptr`（Owned/Affine）+ 裸引用（Borrow）；不能 copy、能借用 `get`、借用活不得比 owner 久。
- Affine vs Linear：`unique_ptr`（可丢）vs `scoped_lock`（必须释放）。
- Place 部分 move：可只 move 走 struct 的一个字段。
- 循环约束：循环体不得改变外层变量所有权状态。

---
## 9. 数据流（AST → 声明收集 → 类型解析 → 语义检查 → 产物）

`Program(AST)` -> `SemanticContext::analyze`（多 pass，声明收集+名义身份+形状）-> 类型解析与推断（改写 AST `inferredType/resultType/resolvedSymbolName` 等）-> 收尾（默认数值 i32、checkUnresolved、materializeInferredTypes）-> AST 已被 Sema 填满 -> `OwnershipChecker`（插入隐式 Free、填 cleanups）-> 交给 MoonIR / 或诊断终止。

---
## 10. C++ 读者新概念清单

- `TypePtr`/类型图（shared_ptr）；`nominalId` 名义身份；作用域符号栈；推断变量（`fresh`）；`unify`/occurs-check；`Affine/Linear`；`Place` 部分 move；自动释放（隐式 `FreeStmt`）；反射（编译期谓词 `type_*`）。

---
## 11. 阅读顺序

1) `src/parser/AST.h`（认识节点）；2) `src/sema/SemanticAnalyzer.{h,cpp}`；3) `src/sema/SemanticContext.cpp` 的 `analyze`；4) `src/sema/SymbolTable.{h,cpp}`；5) `src/sema/TypeResolver`；6) `src/sema/DeclarationCollector`；7) `src/sema/BodyAnalyzer`（analyzeFunction/Stmt/Expr）；8) `src/core/Ownership.h` + `defaultUsageForType`；9) `src/sema/OwnershipChecker`；10) `src/sema/TraitChecker`；11) `src/sema/CompileTimeEvaluator`；12) `src/sema/ControlAnalyzer`。

---
## 12. 相关测试与符号核实

相关测试：`tests/fixtures` 下 ownership_*、lambda_*、iterator_*、enum_match、anonymous / named record、generic_*、concepts、structural_trait_coherence，及 examples/{inference,generic,versioning,fragments}；CMake 层 `tests/semantic_regressions.cmake`、`tests/analysis_protocol.cmake`。

已核实的真实符号：`SemanticAnalyzer::analyze/errors`；`SemanticContext::{analyze, typeIdentity, sourceDeclarationKey, error}`；`SymbolTable::{enterScope, defineLinkage, lookup, lookupDepth}`；`SymbolInfo` / `SymbolKind`；`TypeResolver::{resolveTypeAST, declaredType, instantiateNominal, monomorphize, constrain, resolved, require*, materializeInferredTypes}`；`ConstraintSolver::{fresh, resolve, unify, unifyInternal, contains, requireNumeric, defaultUnconstrainedNumeric, hasUnresolved}`；`DeclarationCollector::declare*`、`isFFIType`、`validateFFIFunction`；`BodyAnalyzer::{analyzeFunction, analyzeStmt, analyzeExpr, analyzeLetStmt, analyzeMatchStmt, analyzeLambdaExpr, analyzeTryExpr, inherentUsageForInitializer}`；`OwnershipChecker::{OwnState, Place, Loan, VarInfo, checkFunction, consume, acquireLoan, isPlaceAvailable, hasConflictingLoan, placesOverlap, collectFreesAtScopeExit, validateLinearScope, validateLinearReturnPath}`；该文件职责完全核实自 `src/sema/OwnershipChecker.h/.cpp`；`TraitChecker::{check, registerImpls, checkConcreteFunction, satisfies}`；`ControlAnalyzer::{analyzeSlotDecl, analyzeSlotInvoke, analyzeApply, selectFragment}`；`CompileTimeEvaluator::{analyzeReflectionCall, analyzeDeclarationReflectionCall, evaluateConstExpr}`；`src/core/TypeSystem.h` 的 `Type/TypeKind/TypeField/TypeVariant/ResourceContract/defaultUsageForType/typeRequiresCleanup`；`luna::ownership::{Relation, Usage, Contract, isMoveOnly, mustConsume, strongerUsage}`。
