# src/sema/Inference.h — 约束求解器（类型推断）

> 一句话定位：`ConstraintSolver` 是 Sema 专用的类型推断状态机：分配推断变量、做 unify（合一）、跟踪数值/布尔约束，最后把未约束的数值变量默认成 i32。

## 这个文件做什么

Luna 的类型推断是「约束求解」风格：遇到 `let x = 1 + y` 时不知道 `x`/`y` 的类型，就先生成一个 inference variable（推断变量）（类似 Hindley–Milner 的 `?T`），随后把两边统一（unify）起来，最后把仍然悬空的变量具体化。本文件声明了 `ConstraintSolver`，它是这一整套机制的核心。

注意：推断变量（`TypeKind::InferenceVar`）不是合法的 MoonIR 类型。Moon 验证器会拒绝任何漏网的前端特化残留，因此 `SemanticContext::analyze` 结束时必须做 `defaultUnconstrainedNumeric()` 与 `checkUnresolved` 清理。

C++ 类比：类似 C++ 模板推导里的「待推导占位类型」（`auto` / `decltype(auto)`）的求解器：变量先挂空，靠约束逐步收窄，最后落地成具体类型。

## 关键结构体·类·枚举

- `class ConstraintSolver`：核心类，私有成员即求解状态：
  - `int mNextId`：下一个推断变量的编号（单调递增）。
  - `unordered_map<int, TypePtr> mBindings`：推断变量 id → 已绑定的类型（可能再指向另一个推断变量，形成链）。
  - `unordered_map<int, bool> mNumericConstraints`：记录某推断变量被要求为数值（`requireNumeric` 打标）。
  - `unordered_map<int, bool> mBoolConstraints`：记录某推断变量被要求为 bool（`requireBool` 打标）。

## 关键函数·方法

公开接口（供 `TypeResolver` 调用）：

- `TypePtr fresh()`：生成一个新的推断变量（`Type::makeInferenceVar(mNextId++)`）。
- `TypePtr resolve(type)`：递归展开绑定链，把推断变量替换成最终类型；同时把中间节点的成员类型（`inner`、`typeArgs`、`fields` 等）也一并 resolve，保证返回的是一棵「已展开」的树。
- `bool unify(lhs, rhs, reason*)`：合一：递归比较两棵树；处理推断变量绑定、occurs-check（`contains` 检测递归约束）、数值/bool 约束传播、名义类型（nominal）id 比较、record/enum/function/slot/fragment 的结构比较等。失败时把原因写进 `reason`。
- `void requireNumeric(type)` / `requireBool(type)`：若目标是推断变量则打上数值/bool 标记（具体类型校验交给 `TypeResolver` 以产生带源码位置的诊断）。
- `void defaultUnconstrainedNumeric()`：遍历所有已分配 id，把仍带 `mNumericConstraints` 且未解析的推断变量绑定为 `TyI32`（数值字面量的确定性默认）。
- `bool hasUnresolved(type)`：递归检查类型树里是否还残留未解析的推断变量（供 `checkUnresolved` 报「无法推断」错误）。

私有实现：`unifyInternal`（真正的合一算法）、`contains`（occurs-check）、`collectUnresolvedNumeric`（收集未解析数值变量）。

## 与周边文件·阶段的关系

- `TypeSystem.h` 的 `#include "Inference.h"` 说明推断与类型图同居一处；`SemanticContext` 持有一个 `ConstraintSolver mConstraints`。
- `TypeResolver`（`TypeResolver.cpp`）是主要调用方：`fresh()` 用于 `declaredType`/`auto`；`unify` 用于 `constrain`；`requireNumeric`/`requireBool`/`requireInteger` 用于操作符检查；`resolve` 用于取值。
- `SemanticContext::analyze` 在收尾阶段调用 `defaultUnconstrainedNumeric()` 并对 `mInferenceRoots` 逐个 `checkUnresolved`，再 `materializeInferredTypes` 把具体类型写回 AST。
- 推断变量绝不允许漏到 MoonIR：Moon 验证器是最后防线。

## 延伸阅读

- `TypeSystem.cpp`：`ConstraintSolver` 的全部实现（`resolve`/`unifyInternal`/`contains`/`collectUnresolvedNumeric` 等）。
- `TypeResolver.cpp`：`constrain`/`requireNumeric`/`requireBool`/`requireInteger`/`checkUnresolved`/`materializeInferredTypes`。
- `SemanticContext.h`：`mConstraints`、`mInferenceRoots` 字段。
- 教科书：类型推断（HM 风格）、约束求解与 occurs-check。


---

---
kind: source-file-guide
module: sema
source: src/sema/OwnershipChecker.cpp
lang: zh-CN
audience: 学过 C/C++（熟悉移动语义/智能指针）、想读 Luna 所有权检查实现的读者
---
