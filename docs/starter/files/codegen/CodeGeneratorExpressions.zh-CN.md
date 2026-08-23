# src/codegen/CodeGeneratorExpressions.cpp —— 表达式层级 LLVM 代码生成（最大文件，1820 行）

## 这个文件做什么

本文件实现 `CodeGenerator` 类中所有以 `generate` 开头的表达式生成方法，以及总调度入口 `generateExpr(Expr*)`，覆盖 Luna 语言的**全部表达式种类**：字面量、标识符、动态选择、字段访问、切片长度、下标索引、二元/一元运算、变体构造/Result 构造/Record 字面量、初始化分配/堆分配、函数调用（含内建/标准库/动态片段/插件/GPU 内建/print/Ok/Err/is_ok/unwrap/slice/panic/compile-time 常量）、try 传播、赋值（含复合赋值 deref/field/index/local）、移动、借用、解引用、取地址、lambda、闭包环境加载、闭包构造。

对 C++ 读者：这是整个 LLVM 后端的「表达式求值器」——每个 `generateXxx` 对应一种 AST 节点，经 `generateExpr` 的 dynamic_cast 分派后，使用 `mBuilder->Create*` 系列生成 LLVM IR。其中 `generateCall` 最庞大（~470 行），需要处理各种内建运行时调用与函数指针/闭包调用的间接调用。

## 关键函数·方法

**`llvm::Value* CodeGenerator::generateExpr(Expr* expr)`（总调度，1819 行）**
- 对 Expr 子类连串 dynamic_cast + 调用对应 generateXxx。若 `generateBinary`/`generateUnary` 返回 nullptr 则继续尝试其他类型（短路逻辑）。最后未知类型报错并返回 PoisonValue。

**字面量生成器**（generateIntLiteral/FloatLiteral/StringLiteral/BoolLiteral/UnitLiteral/ArrayLiteral）：
- `IntLiteral`：`ConstantInt::get(i32Ty, value, true)`。`FloatLiteral`：`ConstantFP::get(f64Ty, value)`。`StringLiteral`：`CreateGlobalString` → GEP 取首字符。`BoolLiteral`：i1 常量。`UnitLiteral`：返回 i32 0（单元无运行时负载）。`ArrayLiteral`：`UndefValue` 数组 → 逐元素 CreateInsertValue。

**访问生成器**（generateIdentifier/DynamicSelect/FieldAccess/SliceLength/Index）：
- `Identifier`：按 canonical local (LocalId) → mLocals (name) → resolveFunction 顺序，返回加载值。
- `DynamicSelect`：对候选按 metadata 逐字段比较（整数/浮点/布尔/字符串），构建 `CreateSelect` 链选择函数指针，匹配计数非 1 则 abort。
- `FieldAccess`：Record 类型用 ExtractValue；Struct 指针型用 GEP + Load。
- `SliceLength`：ExtractValue slice 的字段 1（长度）。
- `Index`：对 Array 或 Slice 做 `rt_array_index_or_abort` 越界检查（除非可静态证明安全），然后 GEP + Load。

**算术生成器**（generateBinary / generateUnary）：
- `Binary`：`&&` 与 `||` 走短路的 CFG（CondBr 分左右），其余加/减/乘/除/余/位运算/比较都按浮点/整型选择合适的 Create 方法。
- `Unary`：负/逻辑非/位非/解引用分派。

**构造生成器**（generateVariantConstruct/ResultConstruct/RecordLiteral/InitAllocation/HeapAlloc）：
- `VariantConstruct`：按 variantName 找索引，在 Enum payload 槽中 MemCpy 各字段，构建 tag+payload 结构。
- `ResultConstruct`：类似结构，tag 为 bool。
- `RecordLiteral`：Struct 类型走指针分配+字段偏移 store；Record 类型用 UndefValue + InsertValue。
- `InitAllocation`：加载规范分配指针，按字段/元素偏移逐一 store 初始化值。
- `HeapAlloc`：调 `rt_alloc` 获取指针，再按构造函数参数 store 初始化。

**`generateCall`（最复杂，660-1130 行）**：
- 依次检查：iterator terminal（Fold/ForEach/Count/Collect）→ `pointer_cast` → `drop_callback` → `Ok`/`Err`/`is_ok`/`is_err`/`unwrap`/`unwrap_err` → `panic` → `slice`(3 参) → 编译期常量 → GPU 内建(`gpu_alloc_i32`/`gpu_free`/`gpu_load_i32`/`gpu_store_i32`/`gpu_copy_from/to_host_i32`) → `print`(i32 或 cstr) → 动态片段(`rt_dynamic_fragment_select/matches/report_unknown_and_abort`) → 插件(`rt_canonical_fragment_plugin_fallback`/`rt_fragment_plugin_report_error_and_abort`) → 全局函数调用（resolveFunction + CreateCall，Never 返回加 Unreachable）→ 间接函数调用（FunctionType 类型）→ 闭包调用（code_ptr + env 结构，首参数 env 指针）。

**其他表达式**：`generateTry`（条件分支 on isOk→失败边 packResultPayload + emitCleanups + CreateRet）、`generateAssign`（复合赋值展开/数组元素/字段/局部）、`generateMove`（可选更新 guarded cursor）、`generateBorrow`（取地址/数组元素/切片元素）、`generateDeref`（Load）、`generateAddrOf`（返回 alloca）、`generateLambda`（生成隐藏函数 + generateControlFlowBody）、`generateEnvLoad`（闭包 env 结构字段 load）、`generateMakeClosure`（生成隐藏函数 + 构造 closure 结构体 {code_ptr, captured...}）。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的核心——表达式层级的 LLVM 生成，被 `CodeGeneratorControlFlow.cpp` 的 `generateControlFlowBody` 调用（LetStmt/ExprStmt/终结器条件）。也间接被 `CodeGeneratorIterator.cpp`（管线中的 map/filter 回调）、`CodeGeneratorGpu.cpp`（launch 参求值）调用。
- 依赖：`CodeGeneratorRangeAnalysis.h`（安全下标证明）、`../core/TypeLayout.h`（布局偏移）、`CodeGenerator.h`/`CGHelpers.h` 的全部成员。
- 调用者：`CodeGeneratorControlFlow.cpp` 与 `CodeGeneratorIterator.cpp` 及 `generateLambda`/`generateMakeClosure` 自身。

## 延伸阅读

1. `CodeGeneratorControlFlow.cpp`——how generateExpr is called from CFG operations。
2. `CodeGenerator.h`——所有 generateXxx 的签名列表。
3. `CodeGeneratorGpu.cpp`——`generateLaunch` 的 GPU 内核启动路径。
4. `CodeGeneratorIterator.cpp`——`generateIteratorTerminal` 与 `emitCallableInvocation`。

---

---
title: src/codegen/CodeGeneratorFunctions.cpp
path: src/codegen/CodeGeneratorFunctions.cpp
阶段: 代码生成 (CodeGen)
角色: 单个函数体的 LLVM 生成（入口/GPU 初始化缀）
语言: C++
---
