# src/sema/SemanticAnalyzer.cpp — 语义分析器的装配

> 一句话定位：构造 `SemanticContext` + 五个分析组件并把它们互相绑定，然后对外转发 `analyze`/`errors`/`symTable`/`declarationReferences`。

## 这个文件做什么

这是 `SemanticAnalyzer` 的极简实现（48 行）：构造函数完成「装配」，其余方法都是转发。真正的语义分析逻辑都在 `SemanticContext.cpp` 及五个组件中。

## 关键结构体·类·枚举

无新增；见 `SemanticAnalyzer.h`。

## 关键函数·方法

- 构造函数：
  - `make_unique<SemanticContext>()`；
  - 依次 `make_unique<BodyAnalyzer>(mContext->bodyAccess())`、`TypeResolver(typeAccess())`、`CompileTimeEvaluator(compileTimeAccess())`、`DeclarationCollector(declarationAccess())`、`ControlAnalyzer(controlAccess())`——每个组件拿到一个「component-scoped access」引用（见 `SemanticContextAccess.h`）；
  - 再 `bindBodyAnalysis`/`bindTypeAnalysis`/`bindCompileTimeAnalysis`/`bindDeclarationAnalysis`/`bindControlAnalysis` 把五个组件的指针回填进 `SemanticContext`，双向引用建立完毕。
- `~SemanticAnalyzer()`：`= default`。
- `analyze(Program*)` → `mContext->analyze(program)`。
- `errors()` → `mContext->errors()`。
- `symTable()` → `mContext->symTable()`。
- `declarationReferences()` → `mContext->declarationReferences()`。

## 与周边文件·阶段的关系

- 编译器驱动调用 `SemanticAnalyzer::analyze` 完成语义分析；内部由 `SemanticContext::analyze` 按多趟驱动五个组件。
- `SemanticContextAccess.h/.cpp` 是这里的「接线」基础：每个组件的构造函数接受对应 `*ContextAccess`。
- 输出（符号表、错误、声明引用）被后续阶段（TraitChecker、OwnershipChecker、MoonIR、IDE）消费。

## 延伸阅读

- `SemanticAnalyzer.h`（接口）。
- `SemanticContext.h/.cpp`（`analyze` 主流程与多趟调度）。
- `SemanticContextAccess.h/.cpp`（组件访问封装）。


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticAnalyzer.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 语义分析入口的读者
---

# src/sema/SemanticAnalyzer.h — 语义分析对外门面（facade）

> 一句话定位：`SemanticAnalyzer` 是 Sema 对外的门面：构造 `SemanticContext` 与五个分析器，暴露 `analyze(Program*)`、错误列表、符号表与声明引用。

## 这个文件做什么

头文件声明了语义分析唯一的对外类 `SemanticAnalyzer`，以及一个数据记录 `ResolvedDeclarationReference`（IDE 跳转用的「源码引用 → 目标链接名」）。`SemanticAnalyzer` 用组合方式把内部结构藏起来：

- 持有 `SemanticContext`（状态中心）。
- 持有五个组件：`BodyAnalyzer`、`TypeResolver`、`CompileTimeEvaluator`、`DeclarationCollector`、`ControlAnalyzer`。
- 对外接口极简：`analyze(Program*)`、`errors()`、`symTable()`、`declarationReferences()`。

C++ 类比：这是典型的 Pimpl/facade：调用方（驱动、CLI）只跟 `SemanticAnalyzer` 打交道，看不见内部五个组件的协作。

## 关键结构体·类·枚举

- `struct ResolvedDeclarationReference`：`sourcePath`/`line`/`column`/`byteLength`（引用位置）+ `targetLinkageName`（目标链接名）。
- `class SemanticAnalyzer`：非拷贝（删除拷贝构造/赋值）；私有成员：`unique_ptr<SemanticContext> mContext` 与五个 `unique_ptr` 分析器。
- 前置声明：`SemanticContext`、`BodyAnalyzer`、`CompileTimeEvaluator`、`ControlAnalyzer`、`DeclarationCollector`、`TypeResolver`（保持头文件轻量）。

## 关键函数·方法

- 构造函数：`make_unique<SemanticContext>()`，用 `mContext->bodyAccess()/typeAccess()/compileTimeAccess()/declarationAccess()/controlAccess()` 构造五个分析器，再 `bindXxxAnalysis` 回绑到 `SemanticContext`（双向引用建立）。
- `~SemanticAnalyzer()`：default（独占指针自动释放）。
- `bool analyze(Program*)`：转发 `mContext->analyze(program)`，返回是否有错误。
- `errors()`：转发 `mContext->errors()`。
- `symTable()`（两个重载）：转发 `mContext->symTable()`。
- `declarationReferences()`：转发 `mContext->declarationReferences()`。

## 与周边文件·阶段的关系

- 是 Sema 的**入口对象**：Parser 之后、MoonIR 验证之前由编译器驱动调用。
- `SemanticAnalyzer::analyze` 内部委托 `SemanticContext::analyze`，后者按多趟（declare → analyze）驱动五个组件（见 `SemanticContext.cpp`）。
- 输出的 `SymbolTable` 与 `declarationReferences` 供后续阶段（OwnershipChecker、MoonIR 生成、IDE）使用。
- 配套的独立检查器 `TraitChecker`/`OwnershipChecker` 不属于本类，由驱动者在主分析后另行调用。

## 延伸阅读

- `SemanticAnalyzer.cpp`（装配）、`SemanticContext.h`（内部状态）、`SemanticContextAccess.h`（组件间访问）。
- 五个组件的头文件：`BodyAnalyzer.h`/`TypeResolver.h`/`CompileTimeEvaluator.h`/`DeclarationCollector.h`/`ControlAnalyzer.h`。


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticContext.cpp
lang: zh-CN
audience: 学过 C/C++、想读 Luna 语义分析主流程的读者
---
