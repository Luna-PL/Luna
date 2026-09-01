# src/sema/BodyAnalyzer.cpp — 体分析与表达式类型检查实现

> 一句话定位：`BodyAnalyzer` 的全部实现（约 3900 行）：函数/结构/枚举/trait/impl 体分析、语句与表达式类型推断、声明族/内置/GPU 启动/选择器等专项处理。

## 这个文件做什么

这是 Sema 中最大的实现文件。它逐节点遍历并注解 AST，核心动作是「给每个表达式算类型、给每个语句建立类型约束、校验各类语义规则」。

覆盖点：

1. 声明体：`analyzeFunction`（含 kernel 校验）、`analyzeStruct`、`analyzeEnum`、`analyzeTrait`、`analyzeImpl`（From/FromIterator/Drop 特判）。
2. 语句：`analyzeStmt`/`analyzeBlock` + 专用方法（`analyzeLetStmt`/`analyzeMatchStmt`/`analyzeForStmt`）。
3. 表达式：`analyzeExpr` + 专用方法（`analyzeCall`/`analyzeMemberCall`/`analyzeIteratorCall`/`analyzeLaunch`/`analyzeSelect`/`analyzeIntrinsicCall`/lambda/Variant/Record/Try）。
4. 返回路径：`statementAlwaysReturns`/`blockAlwaysReturns`。
5. 绑定用法：`inherentUsageForInitializer`/`finalizeBindingUsage`。

## 关键结构体·类·枚举

- 文件内匿名命名空间助手：`hasLayoutDependentTypeParameter`、`genericDropLayoutDependsOnParameter`（Drop 泛型布局依赖判断）。
- 私有成员 `CaptureFrame`（在 .h）与 `mCaptureFrames`（lambda 捕获记录）。
- 方法内部局部结构（如 `analyzeMatchStmt` 内的 `VariantView`、iterator 协议解析局部变量）。

## 关键函数·方法

- `analyzeFunction(decl)`：保存/恢复上下文；建类型参数绑定；`kernel` 特判（不可 extern/constexpr/generic，首参须 `index: i32`，其余参数限 `&device_buffer<T>`）；绑定参数契约；分析 where 约束谓词（须 bool）；`analyzeBlock(体, currentReturnType)`；无显式 return 时 `constrain` 到 `TyUnit`；非 unit 函数所有可达路径必须返回（否则报错）；kernel 返回须 unit。
- `analyzeStruct`/`analyzeEnum`：struct 校验字段类型解析；enum 去重变体名 + 内联递归布局检查（`reachesInlineType`）。
- `analyzeTrait`：把方法签名（含 Self/typeParams 绑定）存进 `mTraitMethods`，创建 `FunctionDecl` 代理。
- `analyzeImpl`：
  - `From` 特判：只允许 `from` 方法、签名须 `from(source)->target`，move-only 源须显式 affine/linear 参数。
  - 通用：检查 trait 方法齐全/无多余方法；逐方法 `analyzeFunction`；按 trait 签名比对参数类型与 ownership 契约、返回类型。
  - `FromIterator` 特判：begin（无参、返回 `affine builder`）/push（`&mut builder`,`affine item`，返回 unit）/finish（`affine builder`→`affine target`）三方法协议校验。
  - `Drop` 特判：`&mut self`、返回 unit、非泛型、无布局依赖；成功则把 `needsDrop`/`dropGlueSymbol` 标记到 `mDeclaredTypes` 对应的所有名义类型。
- `analyzeStmt(stmt, expectedReturn)`：kernel 内禁 host 控制结构（slot/apply/resume/abort/await/free）；按类型分派：slot 声明/调用→`mContext.analyzeSlotDecl/Invoke`，apply→`analyzeApply`，`resume()` 须在 fragment 内且 interceptor 禁 resume，`abort()` 须在 fragment 内，`await` 要求 `Event` 类型，`let`→`analyzeLetStmt`，`return`→约束返回值与线性契约，`if`/`while`→`requireBool` 条件再分析体，`match`→`analyzeMatchStmt`，`for`→`analyzeForStmt`，`expr`→`analyzeExpr`，`free`→分析操作数。
- `analyzeBlock`：`enterScope`/`enterConstScope`/`enterSlotScope` → 遍历语句 `analyzeStmt` → 退出三作用域。
- `analyzeExpr(expr)`：首查字面量（Int→`TyI32`、Float→`TyF64`、String→`TyString`、Bool→`TyBool`）；`IdentifierExpr`→查 `mFunctionFamilies`（家族>1 报歧义，提示用 `select`）+ `lookupSymbol`；其余按类型分派到专用方法（含 lambda/二元/一元/调用/字段/索引/数组/record/variant/try/move/borrow/deref/取址/块/if 表达式/heap/select 等）。
- `analyzeCall`/`analyzeMemberCall`/`analyzeIteratorCall`/`analyzeIntrinsicCall`：函数调用、成员方法（trait impl 方法）、迭代器（protocol/recipe）、内建/intrinsic 的解析与类型约束。公开 `symbols(Name)` 在此推断 Function/Fragment/Struct/Enum/Trait/MetadataSchema catalog kind（generic nominal fail-closed），`symbol_set` terminal 也在此折叠：`.all()` 冻结 SymbolId 顺序，`.all::<M>()` 验证 metadata 顺序，query view 支持 compile-time count/index 和静态展开的 `for`，但不得跨普通 call/return 边界。
- `analyzeLaunch`：`launch kernel(...)` 启动（线程数、参数、事件）。
- `analyzeSelect`：`select target with selector(...)` 的声明族筛选（配合 `CompileTimeEvaluator` 做 selector 求值）。
- `analyzeLetStmt`/`analyzeMatchStmt`/`analyzeForStmt`/`analyzeLambdaExpr`/`analyzeVariantConstructExpr`/`analyzeRecordLiteralExpr`/`analyzeTryExpr`：大型分支拆出的专用方法（let 绑定用法决策、match 变体/绑定类型、for 迭代协议/物料化、lambda 捕获与闭包构建、变体构造/record 字面量、`try` 错误传播）。
- `inherentUsageForInitializer`/`finalizeBindingUsage`：`let` 初始化的固有用法（调用返回/copy/move 源）与显式声明的矛盾校验。
- `statementAlwaysReturns`/`blockAlwaysReturns`：return/`never` 调用/if(有 else 且两支都返回)/match(所有臂)/apply(有 body)/block 的可达返回判定（供函数返回完备性检查）。

## 与周边文件·阶段的关系

- 由 `SemanticContext` 的 `analyzeFunction`/`analyzeExpr`/`analyzeStmt` 等转发调用；是声明收集后「体分析」趟的主力。
- 通过 `BodyContextAccess` 访问 `SemanticContext` 状态；类型推断委托 `TypeResolver`（`constrain`/`requireXxx`/`resolved`）；编译期求值委托 `CompileTimeEvaluator`；slot/apply/片段语句转发 `ControlAnalyzer`。
- 消费 `DeclarationCollector` 登记的 `mImpls`/`mTraits`/`mFromConversions`/`mFromIteratorImplementations`/`mDeclaredTypes`。
- 填充的 AST 字段（`resultType`/`inferredType`/`resolvedSymbolName`/`compileTimeValue` 等）最终由 `TypeResolver::materializeInferredTypes` 收尾后交给 MoonIR。
- kernel 边界检查（`mInKernel`）保证设备代码只含 DeviceMemory 操作，防止 host 结构混入 HSACO。

## 延伸阅读

- `BodyAnalyzer.h`（接口与私有方法声明）。
- `TypeResolver.h/.cpp`（类型约束与收尾）。
- `ControlAnalyzer.h/.cpp`（slot/apply 转发）。
- `CompileTimeEvaluator.h/.cpp`（反射与编译期求值）。


---

---
kind: source-file-guide
module: sema
source: src/sema/BodyAnalyzer.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 函数体/表达式分析的读者
---

# src/sema/BodyAnalyzer.h — 体分析器（BodyAnalysis 实现）

> 一句话定位：`BodyAnalyzer` 实现 `BodyAnalysis`：分析函数/结构/枚举/特征/impl 的体（body），以及所有语句与表达式的类型推断与校验。

## 这个文件做什么

`BodyAnalyzer` 是 Sema 中最大的单个组件：它遍历函数体、结构体字段、枚举变体、trait 方法签名、impl 方法体，对每个 AST 节点做类型检查与推断。它覆盖：

- 函数/结构/枚举/trait/impl 的体分析（`analyzeFunction`/`analyzeStruct`/`analyzeEnum`/`analyzeTrait`/`analyzeImpl`）。
- 语句分析（`analyzeStmt`/`analyzeBlock`）：let/return/if/while/for/match/abort 等。
- 表达式分析（`analyzeExpr`/`analyzeCall`/`analyzeMemberCall`/`analyzeIteratorCall`/`analyzeLaunch`/`analyzeSelect`/`analyzeIntrinsicCall` 等）。
- 返回路径分析（`statementAlwaysReturns`/`blockAlwaysReturns`）。
- 闭包捕获管理（`CaptureFrame` 栈）。
- 绑定用法决策（`inherentUsageForInitializer`/`finalizeBindingUsage`）。

C++ 类比：相当于 C++ 的「语义分析中函数体类型检查」——对每个表达式推导类型（`analyzeExpr`），对每个语句做类型约束（`analyzeStmt`），对函数签名做特化校验（`analyzeImpl` 中的 trait 方法比对）。

## 关键结构体·类·枚举

- `class BodyAnalyzer final : public BodyAnalysis`：唯一公开类型；私有成员 `BodyContextAccess mContext` 与 `vector<CaptureFrame> mCaptureFrames`。
- `struct CaptureFrame`（私有内嵌）：lambda 捕获帧——`lambdaScopeDepth`（lambda 作用域深度）、`captures`（自由变量名列表，按首次引用顺序）。
- `BodyAnalysis` 接口（在 `SemanticContext.h`）：`analyzeFunction/Struct/Enum/Trait/Impl`、`analyzeStmt/Block/Expr/Call/MemberCall/IteratorCall/Launch/Select`、`statementAlwaysReturns`/`blockAlwaysReturns`。
- 类型别名：`ConstValue`、`FromConversion`、`FromIteratorImplementation`（均来自 `BodyContextAccess`）。

## 关键函数·方法

（语义见 .cpp 指南；这里列职责）

- `analyzeFunction(decl)`：保存/恢复上下文（returnType、inFunction、inKernel 等），进入作用域/const/slot 作用域，`analyzeBlock` 函数体，检查返回路径（`SawReturn` 与 `AlwaysReturns`），`checkUnresolved` 参数/返回。
- `analyzeStruct(decl)`：验证字段类型解析（`resolveTypeAST`）。
- `analyzeEnum(decl)`：去重变体名、检查内联递归布局（`reachesInlineType`）。
- `analyzeTrait(decl)`：存储方法签名到 `mTraitMethods`（含 Self/typeParams 绑定），创建 `FunctionDecl` 代理。
- `analyzeImpl(decl)`：校验 trait 方法齐全性、逐方法 `analyzeFunction` 体、按 trait 签名校验参数/返回类型与契约；`From` 特判（`from` 方法形状）；`FromIterator` 特判（begin/push/finish 协议）；`Drop` 特判（`&mut self` 形状、布局依赖、`needsDrop` 标记）。
- `analyzeStmt(stmt, expectedReturn)` / `analyzeBlock(block, expectedReturn)`：按语句类型分派（`LetStmt`/`MatchStmt`/`ForStmt` 等拆出专用方法，`ReturnStmt`/`ExprStmt`/`IfStmt`/`WhileStmt`/`SlotDeclStmt`/`SlotInvokeStmt`/`ApplyStmt` 等直接处理）。
- `analyzeExpr(expr)`：按表达式类型分派（字面量、标识符、二元/一元、`CallExpr` 转 `analyzeCall`、`LambdaExpr` 转 `analyzeLambdaExpr`、`TryExpr`/`VariantConstructExpr`/`RecordLiteralExpr`/`HeapAllocExpr`/`MoveExpr`/`BorrowExpr`/`DerefExpr`/`AddrOfExpr`/`SelectExpr` 等各专用方法）。
- `analyzeCall(call)`：函数调用类型推断、`analyzeIntrinsicCall` 检查内置函数、`analyzeMemberCall`/`analyzeIteratorCall` 分别处理成员方法与迭代器调用。
- `analyzeLaunch(launch)`：GPU 内核启动（`launch kernel(...)`）分析。
- `analyzeSelect(selection)`：`select target with ...` 声明族筛选分析。
- 私有方法：`analyzeLetStmt`/`analyzeMatchStmt`/`analyzeForStmt`/`analyzeLambdaExpr`/`analyzeVariantConstructExpr`/`analyzeRecordLiteralExpr`/`analyzeTryExpr`/`analyzeIntrinsicCall`（各拆出的大型分支）。
- `inherentUsageForInitializer`/`finalizeBindingUsage`：绑定用法决策（`let` 的 initializer 类型与 `move` 源的继承）。
- `statementAlwaysReturns`/`blockAlwaysReturns`：递归检查返回路径（if/match/while/for/return/abort 的完备性）。

## 与周边文件·阶段的关系

- 由 `SemanticContext` 的 `analyzeFunction`/`analyzeExpr`/`analyzeStmt` 等转发调用。
- 通过 `BodyContextAccess` 访问 `SemanticContext` 的几乎所有状态（符号表、types、impls、traits 等）。
- 消费 `DeclarationCollector` 的登记结果（`mImpls`/`mTraits`/`mDeclaredTypes`/`mFromConversions`/`mFromIteratorImplementations`）。
- 分析中触发的编译期求值（元数据、约束、反射）委托给 `CompileTimeEvaluator`（通过 `SemanticContext` 转发）。
- 槽位/片段相关语句（`SlotDeclStmt`/`SlotInvokeStmt`/`ApplyStmt`）转发给 `ControlAnalyzer`。
- 类型推断经由 `TypeResolver`（`constrain`/`requireXxx`/`resolved` 等）。

## 延伸阅读

- `BodyAnalyzer.cpp`（实现）。
- `SemanticContext.h`（`BodyAnalysis` 接口）。
- `TypeResolver.cpp`（类型约束）。
- `ControlAnalyzer.cpp`（槽位/片段语句转发）。
- `CompileTimeEvaluator.cpp`（编译期求值）。


---

---
kind: source-file-guide
module: sema
source: src/sema/CompileTimeEvaluator.cpp
lang: zh-CN
audience: 学过 C/C++、想读 Luna 编译期求值实现的读者
---
