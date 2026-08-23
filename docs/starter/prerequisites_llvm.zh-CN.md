> Document category: implementation note / tutorial
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative（本文是教学向导，不是语言或 API 契约）
> 语言：中文权威版（英文版为占位）

# 前置课 A：LLVM C++ API 速成（面向 C/C++ 开发者）

打开 Luna 的 `src/codegen/` 时，你会看到大量 `llvm::...` 与 `mBuilder->Create...` 调用。很多 C++ 开发者在这里直接放弃——因为 LLVM 的 API 看起来像"一堆奇怪的 Builder + Value + Type"。本文把 LLVM 讲成你熟悉的 C/C++ 概念，之后回去读真实源码就不会晕。

> 一句话：LLVM 把"机器码/汇编"抽象成**指令图 + 控制流图（CFG）**，而 `llvm::IRBuilder` 是把图一点点**焊出来**的“焊接枪”。

## 0. 心智模型：一切都是图（graph）

LLVM 没有"变量"。一个 LLVM 函数 = 一个**控制流图（CFG）**：节点是**基本块（BasicBlock）**，每个基本块是一排有序的 **SSA 指令**；边是**跳转（br / condbr）**。每条指令消费上游的 `Value`，产出新的 `Value`。

你熟悉的 C 代码：
```c
int x = 3;
x = x + 1;
printf("%d", x);
```

对应 LLVM IR（伪码）：
```text
; entry 基本块
%1 = add i32 3, 1       ; 一条指令，结果是一个 i32 值
call i32 @printf(..., %1)
ret i32 0
```

## 1. 核心类比总表

| 你熟悉的 C/C++ 概念 | LLVM 里的叫法 | 常见 C++ 类型 |
|---|---|---|
| `int` / `double`… 类型 | `llvm::Type` | `llvm::Type*` |
| 一个“值”（变量/常量/表达式结果） | `llvm::Value` | `llvm::Value*` |
| 临时结果 | SSA 值（`%数字`） | 也是 `llvm::Value*` |
| 函数里“一段连续代码” | 基本块 `llvm::BasicBlock` | `llvm::BasicBlock*` |
| if/else、循环的分支 | 边 / terminator | `CreateBr` / `CreateCondBr` |
| 局部变量的栈上内存 | `alloca` 指令得到的指针 | `llvm::AllocaInst*` |
| 读写栈上内存 | `load` / `store` | `CreateLoad` / `CreateStore` |
| 访问 `struct.field` | `getelementptr`（GEP） | `CreateGEP` |
| 调用函数 | `call` 指令 | `CreateCall` |
| “往哪个块末尾追加指令” | instruction insertion point | `IRBuilder` |

**核心绕口点**：`Value` 既可以是常量，也可以是某条指令的结果。组合 IR 时基本不用关心“它是常量还是 add 出来的”，它都只是 `Value*`。

## 2. Type：描述“一段数据长什么样”

`llvm::Type` 对应 C 的类型概念，但实例是**全局共享**的（存在 `LLVMContext` 里）。你不“new”一个 i32，而是向 context 借一个：
```cpp
ctx->getInt32Ty();   // i32
ctx->getInt64Ty();   // i64
ctx->getDoubleTy();  // double
ctx->getPtrTy();     // 指针
```

真实 Luna 代码（`src/codegen/CGHelpers.h`）：
```cpp
llvm::Type* i32Ty() const { return llvm::Type::getInt32Ty(mCtx); }
llvm::Type* boolTy() const { return llvm::Type::getInt1Ty(mCtx); }
llvm::Type* sizeTy() const { return llvm::Type::getInt64Ty(mCtx); }
```
> 类比：这是一个“类型 id 单例池”。`LLVMContext` 是全局锚点，所有 `Function`/`Module` 绑定同一个 context。

## 3. Value：一切“运行时数据”的统一接口

无论是一个 `int` 局部变量、一条 `add` 的结果、还是一个函数返回值，LLVM 一律当作一个 `llvm::Value*`。它是很多具体指令类的父类，可用 `Value::getType()` 取类型。

```cpp
llvm::Value* c = llvm::ConstantInt::get(i32Ty, 3);  // 常量 3
llvm::Value* y = mBuilder->CreateAdd(a, b);          // 生成的指令，返回 Value*
```

> 读者练习：为什么 Luna codegen 里到处都是 `llvm::Value*` 而不写具体类型？因为 LLVM 希望你在统一接口（“值”）之上组合，而把具体指令类别藏起来。

## 4. IRBuilder：把 IR“焊出来”

`llvm::IRBuilder<>`（Luna 里记为 `mBuilder`）维护一个“插入点（insertion point）”。你每调用一次 `CreateXxx`，它就在当前插入点之后生成一条 `Xxx` 指令并返回其结果的 `Value*`，同时把插入点往后移一格。

真实 Luna 提取（`CodeGeneratorCleanup.cpp` 等）：
```cpp
// 造一个 alloca（栈上局部），名字叫 "result.payload"
llvm::AllocaInst* slot = mBuilder->CreateAlloca(type, nullptr, name);
// 写内存：store 值到 slot
mBuilder->CreateStore(value, slot);
// 读内存：从 slot load 出来
llvm::Value* v = mBuilder->CreateLoad(slotType, slot);
// 取结构体字段地址（类比 &s.field）
auto* fp = mBuilder->CreateGEP(structType, base, {idx0, idxField});
// 比较 + 条件跳转（类比 if (a == b) { } else { }）
llvm::Value* eq = mBuilder->CreateICmpEQ(actual, expected);
mBuilder->CreateCondBr(eq, thenBlock, elseBlock);
```

要点：**调用顺序就是插入顺序**。所以 Luna 以“自上而下”的过程式风格生成 IR，把抽象语法树/CFG 翻译成一条条指令。

## 5. Function 与 BasicBlock：怎么建一个函数

生成一个函数需要：函数类型（签名）→ `llvm::Function::Create` → 创建基本块并把插入点放进去。

```cpp
auto* ft = llvm::FunctionType::get(retTy, paramTys, /*isVarArg*/false);
llvm::Function* fn = llvm::Function::Create(ft, Linkage, name, module.get());
auto* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
mBuilder->SetInsertPoint(entry);        // 开始往 entry 里焊指令
```

Luna 里生成函数前会先 `SetInsertPoint` 到入口块。`getEntryBlock().begin()` 表示入口块第一条指令之前（用于把 alloca 都提到函数头部，见 `CodeGenerator.cpp` 的 `tmpBuilder`）。

## 6. Alloca / Load / Store：局部变量怎么活

- `alloca` 在栈帧里申请一块内存并返回指针（类比函数顶部声明的局部变量）。
- `store v, p` 把 `v` 写到内存 `p`（`*p = v`）。
- `load p` 把 `*p` 读成一个新 SSA 值。

> 类比：LLVM 里“局部变量”= 一块 alloca 出来的内存 + 每次读写都用 load/store 经过它。因为 SSA 值不可变，需要“可变状态”就必须走内存。

所以 Luna 的函数体生成常见顺序：对每个 Luna 局部变量 `createAlloca` → 用 store 赋初值 → 后续用 load 读最新值。

## 7. GEP：`struct.field` 的算式

要访问一个结构体字段的地址，用 `getelementptr`。它是——不真的“访问”——只**计算**地址。

```cpp
// base 指向某结构体，idx0 是外层，idxField 是里面的字段序
llvm::Value* fieldAddr = mBuilder->CreateGEP(structTy, base, {idx0, idxField});
llvm::Value* value = mBuilder->CreateLoad(fieldTy, fieldAddr);
```

> 常见坑：GEP 不看内存，只看“类型与下标”→ 所以下标必须与类型匹配，否则就是类型错误。这也是为什么 Luna 的 codegen 会先查 TypeLayout（大小/字段）再算 GEP。

## 8. 从 MoonIR 到 LLVM：Luna 的桥接

Luna 拿到的 MoonIR 已经是**密封（sealed）后的规范 CFG**（见 ），codegen 的职责就是把每条 MoonIR 节点/运算映射成 LLVM 指令：

| MoonIR 概念（语义） | LLVM 落点 |
|---|---|
| Local/Binding | alloca + load/store |
| 字段投影（ProjectionKind::Field） | CreateGEP + load |
| 控制流 Jump/Branch/Switch | CreateBr 链接的 BasicBlock |
| TerminatorKind::Return | CreateRet / CreateRetVoid |
| 末尾清理（Cleanup） | 在函数尾/分支处生成 store + drop 调用 |

## 9. 工程基座：Module / Context / JIT

- `llvm::LLVMContext`：全局单例，持有 type id 与 metadata。
- `llvm::Module`：一个“翻译单元”（可类比单个 `.o` 或 LLVM 里的 `.bc`），持有函数、全局变量、底层版。
- `llvm::ExecutionEngine`/ORC `LLJIT`：运行时执行（JIT）。Luna 的 `jitRun()` 走 LLJIT；AOT 走 `emitObjectFile`。

Luna `CodeGenerator` 成员：`std::unique_ptr<llvm::LLVMContext>`、`std::unique_ptr<llvm::Module>`、`std::unique_ptr<llvm::IRBuilder<>>`（即 `mBuilder`）、以及 `LunaOptimizationLevel`。

## 10. 给 C++ 读者的“新概念”清单

- **SSA（Static Single Assignment）**：每个值只被赋值一次（用 `%n` 编号）。等价于 C++ 里“每一次计算都是一个新的 `const` 临时变量”。
- **BasicBlock + CFG**：把 if/else/while 变成显式的块与跳转边。
- **IRBuilder 的插入点**：“在当前位置追加”的 pointer。
- **Value 的双重身份**：同一个 `Value*` 既可能是常量也可能是某条指令的结果，你在编译期并不总是知道，只需在接口上依赖 `Value::getType()`。
- **alloca/load/store**：把“可变变量”变成“内存 + 读写”。

## 11. 继续阅读

- 真实用法：[codegen 子系统导读](./codegen.zh-CN.md)
- 内存/ABI 概念：[前置课 B：ABI / 内存布局](./prerequisites_abi.zh-CN.md)