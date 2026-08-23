# src/sema/CompileTimeEvaluator.cpp — 编译期求值器实现

> 一句话定位：`CompileTimeEvaluator` 的全部实现：反射调用求值、const 作用域、constexpr 函数解释执行、约束/选择器求值。

## 这个文件做什么

这是 Sema 的「迷你解释器」（约 1120 行）：在 AST 上直接求值编译期表达式。它不生成代码，只把编译期可确定的值算出来并写回 `CallExpr::compileTimeValue`/`resultType` 等字段。

## 关键结构体·类·枚举

无新类型；使用 `CompileTimeContextAccess`。文件内大量 lambda 助手（`kindName`、`containsTypeParameter`、`constIndex`、`resolveArgument` 等）。

## 关键函数·方法

- `analyzeReflectionCall(call, name)`：
  - 二元类型关系：`type_same`/`type_same_shape`/`type_abi_compatible`（要求 2 个类型参数），求值写 `compileTimeValue`，返回 `TyBool`。
  - 一元反射：`type_of`/`type_kind`/`type_id`/`type_shape`/`type_domain`/`type_nominal`/`type_size`/`type_alignment`/`type_is_*`/`type_field_count`/`type_field_name`/`type_field_type`/`type_variant_count`/`type_variant_name`/`type_variant_field_count`。
  - 参数形态：`<Type>()` 或单值参数；`type_size`/`type_alignment` 遇类型参数时不冻结（泛型特化前）。
  - 返回类型：计数类 → `TyI32`；`type_is_*` → `TyBool`；其余 → `TyString`。
- `analyzeDeclarationReflectionCall(call, name)`：
  - `declaration_of`：参数须为静态命名标识符；在 `mFunctionFamilies` 中筛选（可选按 callable 签名匹配），唯一性校验；产出 `Type::makeDeclarationRef` 并写 `compileTimeDeclarationId`/`resolvedSymbolName`。
  - `declaration_id`/`declaration_type` 等：参数须是 `declaration_ref`，从嵌套 `CallExpr` 的 `compileTimeDeclarationId` 或符号表的 `compileTimeDeclarationId` 取 id；返回 `TyString`。
- `enterConstScope`/`exitConstScope`：`mConstScopes.emplace_back()`/`pop_back()`（保底根作用域）。
- `defineConst`/`lookupConst`：写/读 const 作用域栈。
- `evaluateConstExpr(expr, locals)`：
  - 字面量直接返回；标识符查 locals 再查全局 const；
  - `CallExpr`：已求值反射（`compileTimeValue`）直接返回，否则查 `mConstexprFunctions` 调 `evaluateConstFunction`；
  - 一元（`-`/`!`/`~`）与二元（算术/位/比较/逻辑）按类型分支求值；不支持返回 `nullopt`。
- `evaluateConstFunction(function, args)`：仅 `isConstexpr`；深度上限 128；绑定参数到 locals，`evaluateConstBlock` 执行。
- `evaluateConstBlock(block, locals, result)`：逐语句：`let` 求值入 locals；`return` 求值出结果；`if` 按条件递归；其他语句失败。
- `evaluateConstraintExpr(expr, bindings, active)`：约束谓词求值：字面量/const 查找、一元、二元（短路 `&&`/`||`）、嵌套约束调用（`evaluateConstraint`）、反射谓词（`type_same` 系列/`type_is_*`/`type_field_count` 等）。
- `evaluateConstraint(name, arguments, active)`：查 `mConcepts`（含限定键回退），绑定类型参数，求谓词；`active` 防循环约束。
- `evaluateSelectorExpr(expr, locals)`：选择器表达式求值：字面量、标识符、元数据字段访问（按 `schemaId` 匹配）、一元/二元、赋值（`=`/`+=` 等）、`declaration_of`、`declaration_count`/`declaration_at`/`declaration_id`/`declaration_signature`、`metadata`/`declaration_has_metadata`、`select_unique`、元数据构造（schema 名调用）、constexpr 函数调用。
- `evaluateSelectorBlock(block, locals, result, returned)`：解释执行 selector 函数体：`let`/`return`/`if`（含 else-if 链）/`for`（遍历 declaration/metadata 视图）/`while`（上限 10000）/`ExprStmt`。
- `evaluateSelectorFunction(function, view, arguments, failure)`：用 `mActiveSelectorView` 上下文执行 selector 函数（失败原因写入 `failure`）。

## 与周边文件·阶段的关系

- `SemanticContext` 的 `analyzeReflectionCall`/`evaluateConstExpr`/`evaluateConstraint`/`evaluateSelectorXxx` 转发到这里。
- `DeclarationCollector::validateMetadata` 依赖 `evaluateConstExpr` 求元数据参数值。
- 约束求值在 `BodyAnalyzer` 分析 where 子句/泛型调用时被触发；selector 求值配合 `src/selector/Selector.h` 的 `DeclarationView`（`mActiveSelectorView`）。
- 常量值写入 `CallExpr::compileTimeValue`（`SemanticConstValue`），供 MoonIR 生成阶段直接使用。

## 延伸阅读

- `CompileTimeEvaluator.h`（接口与类型别名）。
- `src/selector/Selector.h`（声明视图）。
- `SemanticContext.h`（`CompileTimeAnalysis` 接口）。
- `docs/starter/compile_time.zh-CN.md`（编译期特性总览）。


---

---
kind: source-file-guide
module: sema
source: src/sema/CompileTimeEvaluator.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 编译期求值的读者
---

# src/sema/CompileTimeEvaluator.h — 编译期求值器（CompileTimeAnalysis 实现）

> 一句话定位：`CompileTimeEvaluator` 实现 `CompileTimeAnalysis`：反射调用（`type_of` 等）、`const` 绑定、`constexpr fn`、约束（constraint）与选择器（selector）的编译期求值。

## 这个文件做什么

Luna 支持编译期计算：`const` 绑定、`constexpr fn`、元数据参数、约束谓词、以及 `select ... with selector(...)` 的声明族筛选。`CompileTimeEvaluator` 是这一切的求值引擎：

- `analyzeReflectionCall`：`type_of`/`type_id`/`type_size`/`type_field_count` 等内建反射函数（带类型参数的调用）。
- `analyzeDeclarationReflectionCall`：`declaration_of`/`declaration_id`/`declaration_signature` 等声明级反射。
- const 作用域管理：`enterConstScope`/`exitConstScope`/`defineConst`/`lookupConst`。
- 常量表达式求值：`evaluateConstExpr`/`evaluateConstFunction`/`evaluateConstBlock`。
- 约束求值：`evaluateConstraintExpr`/`evaluateConstraint`。
- 选择器求值：`evaluateSelectorExpr`/`evaluateSelectorBlock`/`evaluateSelectorFunction`。

C++ 类比：相当于 C++ 的 constexpr 求值器 + 少量反射（`typeid`/`decltype` 风格）的编译期实现，但运行在 AST 上而非字节码上。

## 关键结构体·类·枚举

- `class CompileTimeEvaluator final : public CompileTimeAnalysis`：唯一公开类型；私有成员 `CompileTimeContextAccess mContext`。
- 类型别名：`ConstValue = SemanticConstValue`（`variant<int64_t, double, bool, string>`），以及 `SelectorDeclarationValue`/`SelectorMetadataValue`/`SelectorDeclarationViewValue`/`SelectorMetadataViewValue`/`SelectorValue`（selector 值域）。
- `CompileTimeAnalysis` 接口（在 `SemanticContext.h`）：反射/const/约束/selector 全套虚方法。

## 关键函数·方法

（语义见 .cpp 指南；这里列职责）

- `analyzeReflectionCall(call, name)`：二元类型关系（`type_same`/`type_same_shape`/`type_abi_compatible`）或一元类型反射；求值写入 `call->compileTimeValue` 并返回 `resultType`（i32/bool/string）。
- `analyzeDeclarationReflectionCall(call, name)`：`declaration_of`（家族内选唯一/按签名匹配，产出 `DeclarationRef`）与其他声明反射（`declaration_id`/`declaration_type` 等）。
- `enterConstScope`/`exitConstScope`：const 作用域栈进出。
- `defineConst(name, value)`/`lookupConst(name)`：定义/查找编译期不可变绑定。
- `evaluateConstExpr(expr, locals)`：在 AST 上求值常量表达式（字面量、标识符、反射结果、constexpr 调用、一元/二元运算）。
- `evaluateConstFunction(function, args)`：调用 `constexpr fn`（深度上限 128，防递归爆炸）。
- `evaluateConstBlock(block, locals, result)`：顺序执行块（let/return/if），产出结果。
- `evaluateConstraintExpr(expr, bindings, active)`：在类型参数绑定下求值约束谓词（短路与/或、反射谓词、嵌套约束）。
- `evaluateConstraint(name, arguments, active)`：按约束名查 `mConcepts` 并绑定类型参数求值（`active` 防递归）。
- `evaluateSelectorExpr(expr, locals)`：selector 表达式求值（字面量、元数据字段访问、`declaration_of`、`declaration_count/at/id/signature`、`metadata`/`declaration_has_metadata`、`select_unique`、元数据构造、constexpr 调用）。
- `evaluateSelectorBlock(block, locals, result, returned)`：selector 函数体解释执行（let/return/if/for/while/expr），`for` 可遍历声明视图与元数据视图，`while` 上限 10000 次。
- `evaluateSelectorFunction(function, view, arguments, failure)`：调用 selector 函数，带 `mActiveSelectorView` 上下文。

## 与周边文件·阶段的关系

- 由 `SemanticContext` 的 `analyzeReflectionCall`/`evaluateConstExpr`/`evaluateConstraint`/`evaluateSelectorXxx` 转发调用。
- 通过 `CompileTimeContextAccess` 访问 `mConstScopes`/`mConstexprFunctions`/`mConcepts`/`mMetadataSchemas`/`mFunctionFamilies`/`mActiveSelectorView` 等。
- `BodyAnalyzer` 在分析 `let const`/元数据参数/约束调用时触发这些求值；`DeclarationCollector::validateMetadata` 用 `evaluateConstExpr` 求元数据参数。
- selector 求值依赖 `src/selector/Selector.h` 的 `DeclarationView`。

## 延伸阅读

- `CompileTimeEvaluator.cpp`（实现）。
- `SemanticContext.h`（`CompileTimeAnalysis` 接口与 const/selector 状态）。
- `src/selector/Selector.h`（声明视图与选择器运行时）。
- `docs/starter/sema.zh-CN.md` 的编译期求值章节。


---

---
kind: source-file-guide
module: sema
source: src/sema/ControlAnalyzer.cpp
lang: zh-CN
audience: 学过 C/C++、想读 Luna 槽位/片段分析实现的读者
---
