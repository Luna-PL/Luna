# src/sema/TypeResolver.cpp — 类型解析与泛型特化实现

> 一句话定位：`TypeResolver` 的全部实现：类型 AST→Type、`auto`/推断变量、泛型函数特化（monomorphize）、类型约束检查与推断结果回写。

## 这个文件做什么

这是 Sema 里「类型」这条线的核心实现（约 1180 行），分几大块：

1. 类型解析：`resolveTypeAST` 把类型 AST 翻译成 `Type`，比 `TypeSystem.cpp::resolveType` 多做了符号表/声明解析（查 `mSymTable`/`mDeclaredTypes`）与名义类型实例化。
2. 泛型特化：`monomorphize` + 文件内私有类 `MonomorphizationCloner`——把泛型函数体的 AST 深拷贝一份，把类型参数替换成具体类型，生成可独立编译的实例。
3. 约束与校验：`constrain`/`requireBool`/`requireNumeric`/`requireInteger`/`checkUnresolved`。
4. 双向转换：`typeToAST`（Type→AST）与 `materializeInferredTypes`（推断完成后回填所有 AST 节点）。

C++ 类比：`monomorphize` ≈ 手工做模板实例化：复制函数体 AST、替换模板参数；`MonomorphizationCloner` ≈ 一个深拷贝器，能复制 Block/Stmt/Expr/Type 的每一个节点。

## 关键结构体·类·枚举

- `MonomorphizationCloner`（文件内私有类）：
  - 持有 `typeBindings`（类型参数名→具体类型）。
  - `cloneBlock`/`cloneStmt`/`cloneExpr`/`cloneType`/`cloneParam`：深拷贝对应 AST 节点，同时把 `inferredType`/`resultType`/`resolvedType` 等字段做类型替换（`substituteNominalType`）。
  - `failure()`：遇到不支持的节点记录失败原因（`fail(category)`）。
  - 支持所有语句/表达式：let/return/if/match/while/for/free/slot/apply、lambda/call/launch/variant/record/try/move/borrow 等。
- `TypeResolver`：唯一公开类，持有 `TypeContextAccess mContext`。

## 关键函数·方法

- `findMatchingImpl`：查 `mImpls[traitId][typeId][methodName]` 返回 `FunctionDecl*`。
- `monomorphize(generic, concreteTypes)`：
  - 构造 `luna::instantiation::Request`（generic 声明 id + 具体类型 id 列表），用 `Instantiator::keyFor` 生成缓存键；命中 `mInstantiatedFunctions` 直接返回。
  - 失败过（`State::Failed`）报错；否则创建 `FunctionDecl` 实例（名字 = `entry.instanceId`），构建类型参数绑定，复制参数/返回类型（替换推断变量与类型参数），用 `MonomorphizationCloner` 克隆函数体。
  - 成功则入 `mGeneratedInstances` 与 `mInstantiatedFunctions` 缓存，并 `Instantiator.complete(requestKey)`。
- `resolveTypeAST`：先查 AST 的 `resolvedType` 缓存（特化后已具体化），再查绑定/符号表/内置名/特殊类型（raw/Result/device_buffer/array/slice/event/metadata_view/declaration_view/declaration_ref），查到名义类型时 `instantiateNominal` 并回写 `resolvedType` 缓存，并 `recordDeclarationReference`（IDE 跳转）。
- `instantiateNominal`：`substituteNominalType` + 记录 `typeArgs`。
- `declaredType`：`auto`→`fresh()`，否则 `resolved(resolveTypeAST(...))`。
- `resolved`：`ConstraintSolver::resolve` 展开；对名义类型刷新声明上的 Drop 资源契约（避免缓存 Rc 形状实例保留过期的 Copy 契约）。
- `constrain`：`never` 放行；`unify` 失败时输出 `Type constraint failed in <context>: <reason>`。
- `requireBool/Numeric/Integer`：推断变量打标记；具体类型不符即报错。
- `checkUnresolved`：`hasUnresolved` 报「无法推断」。
- `typeToAST`：把解析后的 Type 转回 TypeAST（供 `auto` 注解具体化与特化参数）。
- `materializeInferredTypes`：遍历函数/impl 声明、lambda、各类表达式与语句，把所有 `inferredType`/`resultType`/`closureType`/iterator 协议字段替换成 `resolved` 后的具体类型；把 `auto` 注解替换成 `typeToAST` 的结果。

## 与周边文件·阶段的关系

- 由 `SemanticAnalyzer` 注入为 `mTypeAnalysis`；`SemanticContext` 的 `resolveTypeAST`/`constrain`/`requireXxx`/`materializeInferredTypes` 都转发到这里。
- 依赖 `Inference.h`（`ConstraintSolver`）与 `SemanticContext` 的 `mConstraints`/`mDeclaredTypes`/`mImpls`/`mSymTable`/`mInstantiator`。
- `SemanticContext::analyze` 在收尾阶段调用 `defaultUnconstrainedNumeric()` → 逐声明 `checkUnresolved` → `materializeInferredTypes`。
- 产出：被填满的 AST（`inferredType`/`resolvedSymbolName`/具体类型注解）直接交给 MoonIR 生成。

## 延伸阅读

- `TypeResolver.h`（接口）、`SemanticContext.h`（`TypeAnalysis` 与运行期状态）。
- `Inference.h`/`TypeSystem.cpp`（约束求解器）。
- `SemanticAnalysisSupport.h`（`substituteNominalType` 等辅助函数）。
- `DeclarationCollector.cpp`（声明与 `mDeclaredTypes` 填充）。


---

---
kind: source-file-guide
module: sema
source: src/sema/TypeResolver.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 类型解析的读者
---

# src/sema/TypeResolver.h — 类型解析器（TypeAnalysis 实现）

> 一句话定位：`TypeResolver` 实现 `TypeAnalysis` 接口：类型 AST 解析、泛型特化（monomorphize）、推断约束（constrain/requireXxx）、以及把推断结果回写 AST（materialize）。

## 这个文件做什么

语义分析把「类型相关的活」全部委托给一个 `TypeAnalysis` 虚接口，`TypeResolver` 是它的唯一实现。它持有 `TypeContextAccess`（见 `SemanticContextAccess.h`），从而能访问 `SemanticContext` 的状态（`mConstraints`、`mDeclaredTypes`、`mImpls`、`mSymTable` 等）。

这里做的事情包括：解析类型 AST 到 `TypePtr`、实例化泛型名义类型、找到匹配的 trait impl 方法、约束两个类型、强制类型满足 bool/数值/整数要求、检查未解析的推断变量、把 `Type` 转回 AST（`typeToAST`），以及在推断完成后把具体类型写回 AST（`materializeInferredTypes`）。

C++ 类比：类型解析 ≈ 把类型注解字符串解析成内部类型表示并做「模板实例化」；`constrain` ≈ 编译期静态断言式的类型相等性检查。

## 关键结构体·类·枚举

- `class TypeResolver final : public TypeAnalysis`：唯一公开类型。私有成员只有一个 `TypeContextAccess mContext`——所有状态都在 `SemanticContext`，通过 access 引用访问（header-only 的组合模式）。
- `TypeAnalysis` 纯虚接口本身在 `SemanticContext.h` 中声明（包含 `findMatchingImpl`、`monomorphize`、`resolveTypeAST`、`instantiateNominal`、`declaredType`、`resolved`、`constrain`、`requireBool/Numeric/Integer`、`checkUnresolved`、`typeToAST`、`materializeInferredTypes`）。

## 关键函数·方法

（签名见 `SemanticContext.h` 的 `TypeAnalysis`；这里列语义）

- `findMatchingImpl(traitName, typeName, methodName)`：在 `mImpls` 三键表里找实现方法。
- `monomorphize(generic, concreteTypes)`：泛型特化——用 `Instantiator` 缓存请求，克隆 AST（`MonomorphizationCloner` 深拷贝 Block/Stmt/Expr/Type）并把类型参数替换成具体类型，产出 `FunctionDecl` 实例存入 `mGeneratedInstances`/`mInstantiatedFunctions`。
- `resolveTypeAST(ast, bindings)`：解析类型 AST：先查 `resolvedType` 缓存，再查绑定/符号表/内置名/特殊语法类型，最后落到名义类型实例化。
- `instantiateNominal(type, args)`：对泛型名义类型做类型参数替换（`substituteNominalType`）并记录 `typeArgs`。
- `declaredType(ast, bindings)`：`auto` 或缺失类型 → `fresh()` 推断变量，否则 `resolved(resolveTypeAST(...))`。
- `resolved(type)`：用 `ConstraintSolver::resolve` 展开推断变量；对名义类型顺带刷新来自声明的最新 Drop 资源契约。
- `constrain(actual, expected, context)`：`never` 是底类型放行，否则 `unify`，失败产出带上下文的诊断。
- `requireBool/Numeric/Integer`：推断变量打标记，否则直接检查具体类型并报错。
- `checkUnresolved(type, context)`：`hasUnresolved` 时报「无法推断」。
- `typeToAST(type)`：把 `Type` 逆转换回 `TypeAST`（`auto` 注解替换成推断出的具体类型）。
- `materializeInferredTypes(program)`：遍历函数/impl/lambda/表达式/语句，把 `inferredType`、`resultType`、`closureType`、iterator 协议类型等都替换成解析后的具体类型，并给 `auto` 注解补上具体类型 AST。

## 与周边文件·阶段的关系

- 由 `SemanticAnalyzer` 构造，通过 `SemanticContext::bindTypeAnalysis` 注入；`SemanticContext` 的 `resolveTypeAST`/`constrain` 等转发到它。
- 消费 `ConstraintSolver`（`Inference.h`/`TypeSystem.cpp`）做推断；消费 `DeclarationCollector` 写好的 `mDeclaredTypes`/`mImpls`。
- `MonomorphizationCloner` 是本文件内私有助手类，专门深拷贝泛型函数体 AST。
- 特化请求缓存 `luna::instantiation::Instantiator` 在 `SemanticContext` 中。
- `materializeInferredTypes` 是 Sema 最后阶段之一，把 AST 填满后交给 MoonIR。

## 延伸阅读

- `SemanticContext.h`（`TypeAnalysis` 接口、`mConstraints`、`mInstantiator`）。
- `SemanticContextAccess.h`（`TypeContextAccess`）。
- `Inference.h`/`TypeSystem.cpp`（约束求解器）。
- `DeclarationCollector.cpp`（声明与名义类型注册）。


---

---
kind: source-file-guide
module: sema
source: src/sema/TypeSystem.cpp
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 类型解析与推断实现的读者
---
