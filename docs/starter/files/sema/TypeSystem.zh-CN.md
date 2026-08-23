# src/sema/TypeSystem.cpp — 类型 AST 翻译与约束求解实现

> 一句话定位：实现两个核心能力——把类型 AST 直接翻译成 `Type`（`resolveType`），以及 `ConstraintSolver` 的完整推断算法（resolve/unify/默认化）。

## 这个文件做什么

前半部分是全局函数 `resolveType`：纯粹按类型 AST 的语法构造 `Type`，只认内置类型名（`i32`、`Result`、`raw`、`array`、`slice`、`device_buffer` 等）与传入的 `typeBindings`，**不做符号表查询**（那是 `TypeResolver` 的职责）。这相当于类型树的「反序列化」。

后半部分是 `ConstraintSolver` 的全部实现：类型推断的约束求解引擎，负责推断变量的绑定、递归展开（resolve）、合一（unify）与未解析数值的默认化。

C++ 类比：`resolveType` ≈ 把一个 `std::variant`/语法树转成运行时类型对象；`ConstraintSolver::unify` ≈ 把两棵类型树做模式匹配式的结构比较，遇到占位符就建立替换。

## 关键结构体·类·枚举

无新增类型；结构在 `Inference.h`/`core/TypeSystem.h`。这里只有两个实现主体：`resolveType`（自由函数）与 `ConstraintSolver` 的成员函数。

## 关键函数·方法

`resolveType(ast, bindings)`：

- `RecordTypeAST` → `Type::makeRecord`（字段递归翻译）。
- `NamedTypeAST`：先查 `typeBindings`（类型参数替换）；再查内置名（整数/浮点/bool/string/cstr/unit/never/Self）；然后特殊语法类型：`raw<T>`（`makeRawPointer`）、`Result<T,E>`、`device_buffer<T>`、`array<T,N>`、`slice<T>`、`event`、`metadata_view<M>`、`declaration_view`、`declaration_ref`；最后兜底 `Type::makeStruct(name)` + 递归 typeArgs（此时还不知道符号表里的定义，由后续阶段补全）。
- `RefTypeAST` → `makeReference(inner, isMutable)`；`LinearTypeAST`/`AffineTypeAST` 只剥壳返回内层；`FunctionTypeAST` → `makeFunction`。
- 无法识别的返回 `TyUnknown`。

`ConstraintSolver`：

- `fresh()`：`Type::makeInferenceVar(mNextId++)`。
- `resolve(type)`：解绑定链并递归把 `inner`/`typeArgs`/`paramTypes`/`fields`/`variants`/`capturedFields` 等都展开；带路径压缩（把链尾直接写回中间节点）。
- `contains(type, id)`：occurs-check，检测一个推断变量是否出现在某棵树内部（避免递归类型）。
- `unifyInternal(lhs, rhs, reason*)`：核心合一：
  - 一方是推断变量：occurs-check、数值/bool 约束校验与合并、建立绑定；
  - 双方具体类型：比较 domain 与 kind（record/struct 可互视为产品类型）；名义类型（`IdentityMode::Nominal/MetaSchema`）比较 `nominalId` 与 typeArgs；引用比较可变性；raw/device_buffer/metadata_view/iterator 等包装比较 `inner`（iterator 还比较 `iteratorMode`）；`Result` 比较两个 typeArgs；record/struct 按字段名与类型逐一比较；enum 按变体比较；function/slot/fragment 比较 continuationKind/multiShot/参数/返回。
- `unify(lhs, rhs, reason*)`：对外壳，转发给 `unifyInternal`。
- `requireNumeric`/`requireBool`：对推断变量打标记（具体类型校验在 TypeResolver 做）。
- `collectUnresolvedNumeric`：递归找带数值标记的未解析推断变量并绑成 `TyI32`。
- `defaultUnconstrainedNumeric()`：对每个已分配 id 调用上述收集——数值字面量默认 i32。
- `hasUnresolved(type)`：递归检查是否仍有未解析推断变量。

## 与周边文件·阶段的关系

- `TypeSystem.h` 声明，`Inference.h` 声明 `ConstraintSolver`；本文件实现。
- `SemanticContext::analyze` 末尾调用 `mConstraints.defaultUnconstrainedNumeric()` 后做 `checkUnresolved`。
- `TypeResolver::resolveTypeAST` 是带符号表解析的版本，内置名部分基本复用这里的逻辑，但会先查 `mSymTable`/`mDeclaredTypes`。
- `TraitChecker` 等使用 `resolveType` 做纯语法翻译。

## 延伸阅读

- `Inference.h`（约束求解器接口）、`src/core/TypeSystem.h`（`Type` 本体）。
- `TypeResolver.cpp`：真正连接符号表与类型系统的解析路径。
- 教科书：Hindley–Milner 类型推断、合一算法、occurs-check。


---

---
kind: source-file-guide
module: sema
source: src/sema/TypeSystem.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 类型系统的读者
---

# src/sema/TypeSystem.h — 语义分析的类型入口与推断绑定

> 一句话定位：把「规范类型图（Core）与推断求解器（Sema）」粘在一起的头文件：类型图本身在 Core，推断属于 Sema。

## 这个文件做什么

这是 Sema 侧的类型总入口，只有几行，但信息量很大：它明确了一条架构边界——规范类型图（`Type` / `TypeKind` / `Type::makeXxx` / `TyI32` 等）**不归 Sema 拥有**，而是定义在 Core（`src/core/TypeSystem.h`），这样 MoonIR 绝不依赖 Sema 头文件；Sema 这边补充的是「推断与约束求解」能力，即 `Inference.h` 里的 `ConstraintSolver`。

## 关键结构体·类·枚举

- 本文件无自有类型；通过 `#include "../core/TypeSystem.h"` 引入：
  - `Type` / `TypePtr` / `TypeKind`、`Type::makeStruct`/`makeEnum`/`makeFunction`/`makeInferenceVar` 等工厂。
  - 全局单例类型 `TyI32`/`TyF64`/`TyBool`/`TyUnit`/`TyNever`/`TyUnknown` 等。
  - `TypeVec`（`vector<TypePtr>`）、`TypeField`、`TypeVariant` 等辅助结构。
- 通过 `#include "Inference.h"` 引入 Sema 独有的 `ConstraintSolver`（推断变量、unify、数值/布尔约束）。

## 关键函数·方法

本文件没有函数实现；`TypeSystem.cpp` 提供两个全局入口：

- `TypePtr resolveType(const TypeAST*, const unordered_map<string, TypePtr>& bindings)`：把类型 AST 直接翻译成 `Type`（不含符号表/命名解析，仅内置名与结构）。
- `ConstraintSolver` 的全部方法（`fresh`/`resolve`/`unify`/`requireNumeric`/`requireBool`/`defaultUnconstrainedNumeric`/`hasUnresolved`）。

## 与周边文件·阶段的关系

- `SemanticContext.h` 持有 `ConstraintSolver mConstraints`，并在 `analyze()` 收尾做默认化与未解析检查。
- `TypeResolver`（`TypeResolver.h/.cpp`）是 `ConstraintSolver` 的主要使用方，也是真正的「类型解析器」：它调用 `resolveTypeAST`/`resolve`/`constrain` 等完成名字解析 + 推断。
- 顶层类型 AST → `Type` 的纯翻译（无符号表）由 `TypeSystem.cpp::resolveType` 承担；带命名解析的版本在 `TypeResolver.cpp::resolveTypeAST`。
- `TraitChecker` 等独立检查器也直接调用 `resolveType`。

## 延伸阅读

- `src/core/TypeSystem.h`：规范类型图本身。
- `Inference.h` / `TypeSystem.cpp`：推断与 `resolveType` 实现。
- `TypeResolver.h/.cpp`：完整类型解析流程。


---
