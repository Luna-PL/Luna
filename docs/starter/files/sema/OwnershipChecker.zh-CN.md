# src/sema/OwnershipChecker.cpp — 所有权检查实现

> 一句话定位：`OwnershipChecker` 的全部实现（data-flow 式静态检查）：变量 Valid/Moved/Freed 状态机、借用/冲突管理、线性资源消费、分支/循环状态合并，以及自动插入隐式 `free`。

## 这个文件做什么

这是所有权检查的具体算法（约 2000+ 行）。它模拟程序执行的状态迁移：对每个作用域维护变量状态表（`mScopes`）与借用表（`mLoansInScope`），逐语句做「前向传播」，能处理 return 终止路径、if/match/循环分支合并、lambda 捕获、GPU event、fragment 续延的多次 resume/abort。

设计要点（见 .h 注释）：`FlowResult { ok, fallsThrough }` 区分「正常继续」与「return 终止」——被 return 分支消费的资源绝不能与仍可达的代码状态合并；借用（loan）具有词法存活期，在源变量自动释放之前先释放。

## 关键结构体·类·枚举

结构在 `OwnershipChecker.h`：`OwnState`、`Place`、`Loan`、`VarInfo`、`FlowResult`、`CheckerState`。本文件全是这些结构的算法实现。

## 关键函数·方法

入口：

- `check(program, symTable)`：记符号表、收集 `mFragments`、压入根 apply/slot 作用域；遍历声明：`FunctionDecl`（非 extern）与 `ImplDecl::methods` 逐个 `checkFunction`；返回 `mErrors.empty()`。
- `checkFunction(decl)`：参数默认是非拥有视图（`define` 时 `isHeapAllocated` 仅在 Owned 且需清理时设真）；`checkBlock(体)`；fall-through 时 `validateLinearScope`；把 `collectFreesAtScopeExit()` 得到的隐式 `FreeStmt` 追加到体尾（`isImplicit=true`，`action=cleanupActionForType`）。
- `checkBlock(block)`：压入作用域与 apply/slot 栈；逐语句 `checkStmt`；遇到 `!ok` 提前退出；返回终止后语句不可达（`!fallsThrough` 即 break）；`releaseLoansInCurrentScope` 结束本块借用；`validateLinearScope`（仅 fall-through）；收集 `collectFreesAtScopeExit` 并仅在 fall-through 时把隐式 `FreeStmt` 插入块尾；`exitScope` 并返回。
- `checkStmt(stmt)`、`checkExpr(expr)`：分派到各专用方法；`checkLetStmt`/`checkForStmt`（迭代器源所有权）/`checkReturnStmt`（返回时把返回值标注为待释放并清理）/`checkMatchStmt`（臂间状态合并）/`checkAbortStmt`（fragment 内 abort）/`checkCallExpr`（实参消费、借用、跨调用 in-flight）/`checkVariantConstruct`/`checkRecordLiteral`/`checkLaunchExpr`（GPU event 与 in-flight 资源）。
- `checkSlotInvoke(slot)`：解析最内层静态 apply 或默认 fragment。Many（multi-shot）
  时先快照检查 continuation body 一次；若捕获状态被消费
  （`continuationConsumesCapturedState`），该 fragment 不能多次 resume。
- `checkFragment(fragment, slot, multiShot)`：保存当前槽位/片段上下文、进入作用域绑定片段参数；`checkBlock(fragment->body)`；interceptor 在 body fall-through 后再 `checkBlock(slot->continuation)`；收集所有「退出点」`CheckerState`（含 abort/return 路径）；`mergeFallthroughStates` 逐对合并；`restoreState` 回滚；返回以反映是否 fall-through。
- `continuationConsumesCapturedState(before)`：比较快照前后，若某变量从 `Valid` 变非 `Valid` 说明被消费。
- `captureState`/`restoreState`：`CheckerState` 快照/回滚。
- `sameVarState`/`sameLoanState`/`sameApplyState`/`sameSlotState`：比较两套状态是否一致（状态、借用数、in-flight 数、event 资源、moved 位置集合等）用于分支合并判断。
- Loan 管理：`acquireLoan`/`beginInFlightBorrow`/`releaseLoan`/`releaseLoansInCurrentScope`（借用在源自动释放前先释放）、`finishEvent`。
- `consume(place/var, action)`：移动/释放（必要时诊断 use-after-move/重复 free）；`checkWriteTarget`；`extractPlace`/`placesOverlap`/`isPlaceAvailable`/`hasConflictingLoan`（可变借用冲突）/`allDirectFieldsMoved`/`typeOfPlace`。
- `collectFreesAtScopeExit`/`collectFreesAtReturn`/`collectFreesAtFragmentExit`/`validateLinearScope`/`validateLinearReturnPath`：释放收集与线性资源消费校验。
- `usageFromTypeAST`/`isReferenceExpr`/`isDeviceBuffer`/`isEvent`；`lookup`/`define`；`error`/`setDiagnosticLocation`。

## 与周边文件·阶段的关系

- 独立运行于语义分析之后，输入为已填 `inferredType`/ownership 契约的 AST 与 `SymbolTable`。
- 与 `TraitChecker` 同为 Sema 之外的第二道独立检查器（都不在 `SemanticContext` 组件体系内）。
- 用 `luna::ownership::Contract`/`CleanupAction`（`src/core/TypeSystem.h`）与 `resolveType`。
- 插入的隐式 `FreeStmt`（`isImplicit=true`）进入 AST，MoonIR 据此生成析构。

## 延伸阅读

- `OwnershipChecker.h`（接口/数据结构）。
- `src/core/TypeSystem.h` 的 `luna::ownership`。
- `BodyAnalyzer.cpp` 的绑定用法决策。
- 语言文档：`docs/starter/sema.zh-CN.md` 所有权章节。


---

---
kind: source-file-guide
module: sema
source: src/sema/OwnershipChecker.h
lang: zh-CN
audience: 学过 C/C++（尤其了解移动语义/智能指针）、想读 Luna 所有权检查的读者
---

# src/sema/OwnershipChecker.h — 所有权检查器（移动/借用/释放）

> 一句话定位：`OwnershipChecker` 是独立于主语义分析的第二道检查器：在 AST 上做「数据流式」的所有权检查——跟踪每个变量的 Valid/Moved/Freed 状态、借用（loan）与冲突、线性（linear）资源的一次性消费，并自动插入隐式 `free`。

## 这个文件做什么

Luna 的所有权系统类似 Rust 的 affine/linear 类型：Copy 值可复制、Affine 值只能移动一次、Linear 资源（如 event、device_buffer）必须恰好消费一次。`OwnershipChecker` 在语义分析（类型/符号已就绪）之后运行，逐函数/逐块检查：

- 变量状态机：`Valid` → `Moved`/`Freed`，防止 use-after-move、重复 free。
- 借用（loan）记录：共享/可变引用的存活期与冲突（`hasConflictingLoan`）。
- 部分移动：字段级移动（`Place` 前缀重叠判定）与「全字段已移动则整体不可用」。
- 线性资源：作用域退出/返回时收集待释放（`collectFreesAtScopeExit`/`collectFreesAtReturn`），并自动在函数体尾部插入隐式 `FreeStmt`。
- 控制流合并：if/match/loop 的分支状态合并（`mergeFallthroughStates`/`loopPreservesOuterState`），区分「正常继续」与「return 终止路径」（`FlowResult.fallsThrough`）。
- 闭包/lambda 捕获：Copy 本地变量作为捕获候选复制进 lambda 作用域；Affine/Linear/借用捕获被诊断。
- fragment 续延：检查片段体内资源在 resume/abort 路径上的消费（`checkFragment`/`continuationConsumesCapturedState`）。

C++ 类比：相当于用「静态分析器」模拟 C++ 的 unique_ptr 语义：谁持有、何时移动、何时释放、借用期间能否写。`Place` 类似「存储位置路径」；`Loan` 类似「活跃引用」。

注意：本类不参与 `SemanticContext` 组件体系（不实现任何 `*Analysis` 接口），由驱动者单独构造并调用 `check(Program*, SymbolTable&)`。

## 关键结构体·类·枚举

- `enum class OwnState { Valid, Moved, Freed }`：变量状态。
- `struct Place`：存储位置：`root`（变量名）+ `components`（字段名/常量下标/`[*]`/`*` 解引用）；前缀重叠是部分移动与借用冲突判断的基础。
- `struct Loan`：一条活跃借用：`Place source` + `isMutable`。
- `struct VarInfo`：单个变量的完整所有权状态：`state`、`type`、`isHeapAllocated`、`usage`（Copy/Affine/Linear）、`relation`（Owned/Borrowed）、`movedPlaces`（已移动字段）、`isReference`/`isMutableReference`、`sharedBorrows`/`mutableBorrow`、`inFlightReads`/`inFlightWrites`、`isGpuEvent`、`materializedIteratorOwnsSource`/`materializedIteratorSourceType`、`eventResources`（event 关联资源）、`name`。
- `struct FlowResult { bool ok; bool fallsThrough; }`：块检查结果：`ok` 是否成功、`fallsThrough` 是「正常继续」还是「return 终止」——资源被 return 分支消费时不能与仍可达的代码合并。
- `struct CheckerState`：作用域/借用/apply/slot 四组栈的快照（用于分支保存恢复与合并）。
- `class OwnershipChecker`：私有成员 `mScopes`、`mLoansInScope`、`mErrors`、`mSymTable*`、诊断位置、`mFragments`、`mApplyScopes`/`mSlotScopes`、`mCurrentSlotContinuation`、`mValidatingManyContinuation`、`mCheckingSlotContinuation`、`mCurrentFragmentAbortExits`、片段作用域基址、`mUnavailableLambdaCaptures`。

## 关键函数·方法

入口：

- `check(Program*, SymbolTable&)`：注册片段，逐函数/impl 方法调 `checkFunction`；返回 `mErrors.empty()`。
- `errors()`：只读诊断列表。

主流程：

- `checkFunction(decl)`：建参数（默认借用视图；显式 affine/linear 才转移所有权）；`checkBlock(体)`；`releaseLoansInCurrentScope`；`validateLinearScope`（fall-through 时线性资源必须消费）；必要时在体尾插入隐式 `FreeStmt`（`collectFreesAtScopeExit`）。
- `checkLambda(lambda)`：保存/隔离外层状态，Copy 捕获复制进 lambda 作用域，Affine/Linear/借用捕获报 C016 CL005 类诊断。
- `checkBlock(block)`：进入作用域，逐语句 `checkStmt`，退出时 `releaseLoansInCurrentScope` + `collectFreesAtScopeExit` 自动释放。
- `checkStmt(stmt)`：分派：let/return/match/for/abort/slot invoke/apply/if/while/expr 等（大分支拆出专用方法）。
- `checkExpr(expr)`：表达式级检查（调用、变体构造、record、launch 等专用方法）。
- 专用方法：`checkLetStmt`（绑定状态/用法）、`checkForStmt`（迭代器源所有权）、`checkReturnStmt`（返回路径释放）、`checkMatchStmt`（臂状态合并）、`checkAbortStmt`、`checkCallExpr`（实参消费/借用）、`checkVariantConstruct`/`checkRecordLiteral`、`checkLaunchExpr`（event 与 in-flight 资源）、`checkSlotInvoke`/`checkFragment`（续延资源消费）。

状态管理：

- `enterScope`/`exitScope`/`releaseLoansInCurrentScope`。
- `acquireLoan(place, isMutable)`/`beginInFlightBorrow(name, isMutable)`/`releaseLoan`：借用获取与释放（词法存活期，借用在源变量自动释放前先释放）。
- `consume(place/var, action)`：移动/消费（检查状态与冲突借用）。
- `checkWriteTarget(expr)`：写目标检查（in-flight 读写、冲突借用）。
- `extractPlace(expr)`/`renderPlace`/`renderProjection`/`placesOverlap`/`isPlaceAvailable`/`hasConflictingLoan`/`allDirectFieldsMoved`/`typeOfPlace`：Place 推导与冲突查询。
- `captureState`/`restoreState`/`mergeFallthroughStates`/`loopPreservesOuterState`/`sameVarState`/`sameLoanState`/`sameApplyState`/`sameSlotState`/`describeControlFlowDifference`：分支/循环状态快照、合并与差异诊断。
- `usageFromTypeAST`/`isReferenceExpr`/`isDeviceBuffer`/`isEvent`。
- `lookup`/`define`：变量表读写。
- `collectFreesAtScopeExit`/`collectFreesAtReturn`/`collectFreesAtFragmentExit`/`validateLinearScope`/`validateLinearReturnPath`：释放收集与线性校验。
- `error`/`setDiagnosticLocation`：诊断。

## 与周边文件·阶段的关系

- 独立于 `SemanticContext` 组件体系；在语义分析（类型/符号绑定）完成后运行，消费已填好的 `FunctionDecl::inferredType` 等。
- 直接接收 `SymbolTable&`（与 `SemanticContext` 共享同一符号表）。
- 使用 `src/core/TypeSystem.h`（`resolveType`/`TypeKind`）与 `luna::ownership`（`Usage`/`Relation`/`Contract`/`parameterContractFor`/`defaultUsageForType`）。
- 自动插入的 `FreeStmt`（`isImplicit = true`）会进入 AST，供 MoonIR 生成显式析构调用。
- `TraitChecker` 类似：同为 Sema 之外的第二道独立检查器。

## 延伸阅读

- `OwnershipChecker.cpp`（实现）。
- `src/core/TypeSystem.h` 的 `luna::ownership` 命名空间（Usage/Relation/Contract）。
- `BodyAnalyzer.cpp`（`analyzeLetStmt` 等绑定用法决策，所有权检查的输入）。
- 语言特性背景：`docs/reference/` 中 ownership/linear 设计。


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticAnalysisSupport.h
lang: zh-CN
audience: 学过 C/C++、想了解 Sema 通用工具函数的读者
---
