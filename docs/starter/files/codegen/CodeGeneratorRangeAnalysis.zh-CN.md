# src/codegen/CodeGeneratorRangeAnalysis.cpp —— 数组索引上界推导与安全性的实现

## 这个文件做什么

实现 `CodeGeneratorRangeAnalysis.h` 声明的两个纯函数：`knownArrayIndexUpperBound`（推导 Luna 索引表达式的排他上界）与 `isProvablySafeArrayIndex`（判断索引是否必然不超过给定长度）。目的：让代码生成器在「可静态证明」的前提下省略冗余的安全数组越界检查。

对 C++ 读者：这是代码生成器内部的简化「区间证明子」。它刻意只认识三类容易证明的形态——非负整数常量、已记录上界的局部名、非负掩码 `x & mask`——其余一律返回『无法证明』，从而保证安全。

## 关键结构体·类·枚举

本文件无自有类型。使用 `using moon::BinaryExpr; using moon::IdentifierExpr; using moon::IntLiteralExpr; using moon::Operator;` 简化 AST 类型名，并在命名空间 `luna::codegen` 内实现两个自由函数。

## 关键函数·方法

**`std::optional<uint64_t> knownArrayIndexUpperBound(const moon::Expr* expression, const std::unordered_map<std::string,uint64_t>& knownUpperBounds)`**
- `IntLiteralExpr`（整数常量）：负值返回 nullopt；非负 `v` 返回 `v+1`（排他上界）。
- `IdentifierExpr`（局部名）：在 `knownUpperBounds` 字典按名字查找；查到返回其上界。
- `BinaryExpr`：仅当 `op == Operator::BitAnd` 且 RHS 是非负 `IntLiteralExpr` 时返回 `rhs+1`（因为 `x & mask` 恒在 `[0, mask]` 内，即使 x 带符号）。其余情形返回 nullopt。
- 谁调用：`isProvablySafeArrayIndex`；谁被调：只读 moon::Expr AST，不触碰 IR。

**`bool isProvablySafeArrayIndex(const moon::Expr* expression, uint64_t length, const std::unordered_map<std::string,uint64_t>& knownUpperBounds)`**
- 调用上面函数取上界，返回「上界存在且 bound <= length」。
- 谁调用：`CodeGeneratorExpressions.cpp` 的安全数组下标路径（generateIndex / 相关借用路径），用于跳过冗余 `rt_array_index_or_abort` 运行时检查。

设计要点：文件顶部注释强调「非负位与掩码即使源为带符号也有静态上界」，其余表达式一律保留运行时检查，安全绝不由不完整的区间证明推出——保守且安全优先。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的优化助手。
- 依赖 `../moonir/MoonIR.h`（moon::Expr、moon::BinaryExpr 等）与标准库 `<optional>`/`<string>`/`<unordered_map>`。
- 头文件被 `CodeGeneratorExpressions.cpp` include；`CodeGenerator` 用 `mLocalKnownUpperBounds` 提供已知上界字典。

## 延伸阅读

1. `CodeGeneratorRangeAnalysis.h`——函数声明与语义注释。
2. `CodeGeneratorExpressions.cpp` 的 `generateIndex`/`generateBorrow`——安全下标与越界检查。
3. `CodeGenerator.h` 的 `mLocalKnownUpperBounds`——已知上界的来源与失效规则。

---

# src/codegen/CodeGeneratorRangeAnalysis.h —— 数组索引界限分析的纯函数头文件

## 这个文件做什么

`CodeGeneratorRangeAnalysis.h` 供代码生成器判断「数组索引是否可静态证明安全」。它声明了两个纯函数（在 `luna::codegen` 命名空间中）：一个用于推导某个 Luna 表达式索引的上界，另一个据此判断该索引是否必然落在给定长度之内。后端用它**消除冗余的运行时越界检查**，且绝不从「不完整的证明」推断安全性。

对 C++ 读者：这是典型的「编译期轻量区间分析（range analysis）」接口。它不产生 IR，只回答「这个索引有没有已知上界？是否必然不越界？」两个布尔型问题，是优化中间端的查询助手。

## 关键结构体·类·枚举

本文件不含结构体/类/枚举，只暴露两个命名空间级自由函数：
- `std::optional<uint64_t> knownArrayIndexUpperBound(const moon::Expr* expression, const std::unordered_map<std::string,uint64_t>& knownUpperBounds)`：若已知则返回该表达式的「排他上界」（upper bound，即最大值+1）。
- `bool isProvablySafeArrayIndex(const moon::Expr* expression, uint64_t length, const std::unordered_map<std::string,uint64_t>& knownUpperBounds)`：当上界存在且 `bound <= length` 时返回 true。

参数 `knownUpperBounds` 是「局部变量名 -> 排他上界」的字典，来自 `CodeGenerator` 的 `mLocalKnownUpperBounds`。

## 关键函数·方法

**`knownArrayIndexUpperBound`**（实现在 CodeGeneratorRangeAnalysis.cpp）。对三类输入求上界：
- `IntLiteralExpr`：非负字面量 `v` 的上界为 `v+1`；负字面量返回 `nullopt`。
- `IdentifierExpr`：在 `knownUpperBounds` 字典中按名字查已有上界。
- `BinaryExpr（详见 .cpp）`：仅当 `rhs` 是非负字面量，且运算符为 `BitAnd`（与运算）时返回 `rhs+1`，因为 `x & mask` 必然落在 `[0, mask]`。
谁调用：`isProvablySafeArrayIndex`。谁被调：只读 AST（moon::Expr），不使用 IR。

**`isProvablySafeArrayIndex`**：调用上面的函数取得上界，比较 `bound <= length`。返回 true 时调用方可以省略运行时越界检查。
 谁调用：`CodeGeneratorExpressions.cpp` 中安全下标相关路径（用于跳过冗余 `rt_array_index_or_abort` 检查）。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的辅助优化。
- 依赖 `../moonir/MoonIR.h`（moon::Expr 及其子类）。
- 头文件在 `CodeGeneratorExpressions.cpp` 顶部被 include，后者在安全数组下标处调用本函数决定是否跳过运行期检查。
- 语义约束：只有证明成立才跳过检查，杜绝「incomplete range 推断出来的安全」，是安全优先的保守策略。

## 延伸阅读

1. `CodeGeneratorRangeAnalysis.cpp`——两个函数的实现细节。
2. `CodeGeneratorExpressions.cpp 的 generateIndex/generateBorrow（安全下标路径）。
3. LLVM 运营商：位与（BitAnd）在 MIR 层对应的 Operator 枚举。

---

---
title: src/codegen/CodeGeneratorRuntimeDescriptors.cpp
path: src/codegen/CodeGeneratorRuntimeDescriptors.cpp
阶段: 代码生成 (CodeGen)——运行时声明描述符发射
语言: C++
---
