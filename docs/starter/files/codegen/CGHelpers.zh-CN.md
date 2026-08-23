# src/codegen/CGHelpers.cpp —— Luna 类型到 LLVM 类型的转换与内存尺寸计算实现

## 这个文件做什么

本文件实现 `CGHelpers.h` 中声明的全部内容：`CGHelpers::toLLVMType`（Luna 类型翻译为 LLVM 类型）、文件级自由函数 `typeSize`（字节大小）与 `typeAlignment`（对齐）。它不含任何 IR 生成逻辑，是一个纯粹的「类型到 LLVM 类型 + 内存布局」映射层，被整个代码生成后端当作公共工具调用。

对 C++ 读者：这个文件就像一个「反射式映射表」——用 `switch(type->kind)` 把前端种类（TypeKind）摊平成 LLVM 的对应类型，并维护一份手写的「每种类型占几个字节、按几字节对齐」的尺寸表，供后端在分配、释放、拷贝时换算偏移。

## 关键结构体·类·枚举

本文件的实现对象均来自头文件/外部：
- `CGHelpers` 类及其私有成员 `llvm::LLVMContext& mCtx`（在本文件仅实现其构造函数与 `toLLVMType`）。
- 两个自由函数 `typeSize`、`typeAlignment`。
- 依赖的枚举 `TypeKind`（Luna 前端类型种类，来自 `../core/TypeSystem.h`）。

## 关键函数·方法

**`void* CGHelpers::toLLVMType`**（见头文件签名）
- 按 `type->kind` 分派。要点实现细节：
- `Result` 取 `typeArgs[0]`（值型）与 `typeArgs[1]`（错型）中较大的 `lua::layout::valueSize`，向上取整到 8 字节字，构造 `{ bool tag, [N x i64] payload }`。
- `Enum` 用 `lua::layout::enumPayloadSize(type)` 定宽 payload，构造 `{ i32 tag, [N x i64] payload }`。
- `Array` 递归元素类型得到 `[arrayLength x inner]`；`Slice` 为 `{ptrTy, sizeTy}`；`Record` 逐字段 `toLLVMType` 后 `StructType::get`；`Closure` 首字段为代码指针、其余为捕获字段。
- 谁调用/谁被调：被所有 codegen 文件用于取 LLVM 类型；内部仅递归调用自身与 LLVM 类型构造。

**`uint64_t typeSize(const TypePtr& type)`**
- 基本整数/浮点/布尔按宽度返回 1/2/4/8；`String`/`CStr`/各类指针/`Reference`/`Iterator` 返回 8（指针大小）；`Slice` 返回 16；`Event` 返回 4；`Array` 为 `arrayLength * typeSize(inner)`；`Struct`/`Record`/`Closure`/`Enum`/`Result` 转交 `lua::layout::*`；`Never` 返回 0。
- 谁调用：`generateHeapAlloc`（`rt_alloc` 的 size 参数）、`CodeGeneratorCleanup.cpp` 的 `emitLunaDeallocation` 与部分释放逻辑、`CodeGeneratorControlFlow.cpp` 的 `AllocateStmt`。

**`uint64_t typeAlignment(const TypePtr& type)`**
- 标量按宽度给 1/2/4/8；`Array` 取元素对齐；`Record` 取字段对齐最大值；`Closure` 以 8 为底取捕获字段最大值；`Result`/`Enum`/默认都对齐到 8；`Unit`/`Never` 对齐 1。
- 谁调用：同 `typeSize`，与 `typeSize` 成对传给分配的 rt 函数。
 谁调用（上层）：整个 codegen 后端；谁被调（下层）：`lua::layout::*`（`../core/TypeLayout.h`）。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的类型/布局工具实现。
- 依赖 `../core/TypeLayout.h` 的 `valueSize`/`enumPayloadSize`/`productStorageSize` 等布局函数；这些函数决定了 Luna 值在内存中的具体排布，本文件只是把「尺寸/对齐」暴露给后端。
- 与 `CGHelpers.h` 一一对应（声明在前者，实现在后者）。较底层，被 Module/Functions/ControlFlow/Expressions/Cleanup/Gpu 都间接依赖。

## 延伸阅读

1. `../core/TypeLayout.h` 的布局语义（值大小/字段偏移/枚举载荷大小）。
2. `CGHelpers.h`——对应接口声明与成员说明。
3. `CodeGeneratorCleanup.cpp` 中的 `emitLunaDeallocation`——本文件尺寸计算结果的实际消费方之一。

---

---
title: src/codegen/CGHelpers.h
path: src/codegen/CGHelpers.h
阶段: 代码生成 (CodeGen)
角色: LLVM 类型映射辅助工具头文件
语言: C++
---

# src/codegen/CGHelpers.h —— Luna 类型到 LLVM 类型映射的唯一权威辅助类

## 这个文件做什么

`CGHelpers.h` 是代码生成阶段的轻量「类型工具助手」头文件。它声明了 `CGHelpers` 类，把 Luna 的前端类型系统（`TypePtr`，来自 `../core/TypeSystem.h`）翻译成 LLVM 类型（`llvm::Type*`），并提供一组常用标量 LLVM 类型快捷访问器（i32/i64/f32/f64/bool/void/ptr/size）。同时声明两个文件级自由函数 `typeSize` 与 `typeAlignment`，返回 Luna 类型在内存中的字节大小与对齐值。

对 C++ 读者：可把 `CGHelpers` 类比为「类型工厂 + 上下文持有者」。它借用一个 `llvm::LLVMContext&`（只借不拥有），所有方法都是只读查询，不发射任何 IR，因此可被整个后端安全共享——`CodeGenerator` 通过成员 `std::unique_ptr<CGHelpers> mHelpers` 持有它。

## 关键结构体·类·枚举

**class `CGHelpers`**
- 构造函数 `explicit CGHelpers(llvm::LLVMContext& ctx)`：只保存传入的 LLVM 上下文引用。
- 私有成员 `llvm::LLVMContext& mCtx`：对 LLVM 全局上下文的借用引用，所有类型查询经由它。
- 对外只读接口（内联 const）：核心为 `llvm::Type* toLLVMType(const TypePtr& type) const`；快捷访问器有 `i32Ty()/i64Ty()/f32Ty()/f64Ty()/boolTy()/voidTy()/ptrTy()/sizeTy()`（sizeTy 在 64 位目标等价 i64Ty，ptrTy 为 addrspace 0 通用指针）；还有非 const 的 `context()` 返回上下文引用。

**Free functions（定义在 CGHelpers.cpp）**
- `uint64_t typeSize(const TypePtr&)`：返回该 Luna 类型的字节大小。
- `uint64_t typeAlignment(const TypePtr&)`：返回该 Luna 类型的对齐要求。

## 关键函数·方法

**`llvm::Type* CGHelpers::toLLVMType(const TypePtr& type) const`**
作用：按 `type->kind` 分派，把 Luna 类型映射为 LLVM 类型。要点：
- 整数/浮点/布尔直接映射：I8 转 i8、U32 转 i32、F32 转 f32、Bool 转 i1。
- `String`/`CStr`/`RawPointer`/`DeviceBuffer`/`Metadata`/`MetadataView`/`DeclarationView`/`DeclarationRef`/`Iterator` 都转通用指针 `ptrTy()`。
- `Result` 与 `Enum` 转带标签结构 `{ tag, [N x i64] }`（tag 为 i32 或 bool，payload 按 `lua::layout::valueSize`/`enumPayloadSize` 定宽到 8 字节字）。
- `Array` 转 `[len x inner]`；`Slice` 转 `{ptr, i64}`；`Record` 逐字段递归 `StructType`；`Closure` 转 `{code_ptr, captured...}`；`Reference`/`Struct`/`Function` 转指针；`Unit`/`Never` 转 void；`Event` 转 i32；未知种类回退 i32。
- 谁调用：贯穿 codegen——`CodeGeneratorModule.cpp` 的 `declareFunc`、`CodeGeneratorFunctions.cpp`、`CodeGeneratorControlFlow.cpp`、`CodeGeneratorExpressions.cpp` 等。
- 谁被调：底层调用 LLVM 类型构造函数（`llvm::Type::get*Ty`、`StructType::get`、`ArrayType::get`、`PointerType::get`）。

**`typeSize` / `typeAlignment`（自由函数）**
基础类型直接返回常量；`Record`/`Struct`/`Closure`/`Enum`/`Result` 转交 `lua::layout::*`；`Array` 递归乘以元素个数。被 `CodeGeneratorCleanup.cpp`（释放参数）、`CodeGeneratorControlFlow.cpp`（rt_alloc/rt_dealloc 的 size/align 实参）、`CodeGeneratorExpressions.cpp`（`generateHeapAlloc`）调用。

## 与周边文件·阶段的关系

- 属**代码生成（Codegen）阶段**的底层基础设施。
- 依赖 `../core/TypeSystem.h`（TypePtr、TypeKind）与 `../core/TypeLayout.h`（lua::layout 布局函数）。
- 被 `CodeGenerator.h/.cpp`（以 mHelpers 成员持有）及 Module/Functions/ControlFlow/Expressions/Cleanup 等上游使用。
- 与 `CodeGeneratorRangeAnalysis`、`CodeGeneratorGpu` 不同：只做类型翻译，不生成 IR 指令。

## 延伸阅读

1. 类型布局与大小：`../core/TypeLayout.h` / `.cpp`（valueSize、productFieldOffset、variantFieldOffset、enumPayloadSize）。
2. `CodeGenerator.h`——CodeGenerator 如何持有 mHelpers 并编排后端。
3. LLVM 文档：`llvm::Type`、`llvm::StructType`、`llvm::ArrayType`、`llvm::PointerType`。

---

---
title: src/codegen/CodeGenerator.cpp
path: src/codegen/CodeGenerator.cpp
阶段: 代码生成 (CodeGen)
角色: CodeGenerator 共享小工具方法的实现
语言: C++
---
