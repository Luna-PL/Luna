# src/codegen/CodeGeneratorCleanup.cpp —— 资源清理（Drop/Dealloc/Result/Enum/Array/Record）的全部 LLVM IR 生成

## 这个文件做什么

本文件实现 Luna 运行时的资源清理（drop 与 deallocation）在 LLVM IR 层面的完整生成逻辑。它覆盖了从「规范 CFG 清理记录」（`emitCanonicalCleanup`）到「具体内容清理」（`emitResourceContentsCleanup`）、「所有权负载清理」（`emitOwnedPayloadCleanup`）、「堆分配解除」（`emitLunaDeallocation`）、「Result/Enum 的 tag 分叉清理」、「物化迭代器源清理」（`emitMaterializedIteratorCleanup`）、「按名称/动作清理」（`emitCleanup`）以及「动态 drop 回调」（`getOrCreateDropCallback`）的全套路径。同时提供 `packResultPayload`/`unpackResultPayload` 在 Result/Enum 的 tag + payload 间打包/拆包。

对 C++ 读者：这是 Luna 的「析构函数生成器」。它不依赖 C++ 的 RAII，而是依据 Luna 的所有权分析（`luna::ownership::CleanupAction`）在 CFG 的每个退出点发射显式的释放/解构代码——类似 Rust 的 Drop glue 生成器。

## 关键函数·方法

**`llvm::Function* CodeGenerator::getOrCreateDropCallback(const TypePtr& type)`**
- 对给定类型，按 `typeId -> stableIdentityHash` 生成唯一符号 `__luna_drop_callback_<hash>` 的函数，内部调用 `emitOwnedPayloadCleanup` 处理值清理。缓存到 `mDropCallbacks`。
- 谁调用：`generateCall` 中处理 `drop_callback` 内建时。谁被调：`emitOwnedPayloadCleanup`。

**`void emitLunaDeallocation(Value* pointer, const TypePtr& type)`**
- 调用 `rt_dealloc`（ptr, size, alignment），尺寸来自 `typeSize`/`typeAlignment`。
- 谁调用：`emitOwnedPayloadCleanup` 中对指针型类型的释放。

**`Value* packResultPayload(Value* value, const TypePtr& type, const TypePtr& resultType)`** / **`Value* unpackResultPayload(Value* bits, const TypePtr& type, uint64_t byteOffset)`**
- `packResultPayload`：在 Result 结构的 payload 槽（`[N x i64]`）中，通过 MemCpy 把值按 `lua::layout::valueSize` 拷贝到对齐的临时存储后取出。
- `unpackResultPayload`：从 payload 槽按 byteOffset 读取目标类型值。
- 谁调用：`generateResultConstruct`/`generateVariantConstruct`/`generateTry`/`generateCall`(Ok/Err/unwrap) 与 switch 清理。

**`void emitResourceContentsCleanup(Value* value, const TypePtr& type, const string& label)`**
- 递归清理资源内容：对 `needsDrop` 资源调 `type->dropGlue` 函数；对 `Struct`/`Array`/`Record`/`Closure`/`Result`/`Enum` 分别展开其字段/元素/变体，逐一递归调用 `emitOwnedPayloadCleanup`。Enum 用 `switch` 按 tag 分派变体清理。
- 谁调用：`emitOwnedPayloadCleanup`。谁被调：`resolveFunction`(dropGlue)、`emitOwnedPayloadCleanup`(递归)。

**`void emitOwnedPayloadCleanup(Value* value, const TypePtr& type, const string& label)`**
- 入口：`typeRequiresCleanup` 返回 false 则直接返回。`String`/`CStr` 是 no-op（不可变全局常量，目前无堆分配文本）。`DeviceBuffer` 调 `rt_gpu_free`。`Array`/`Record`/`Result`/`Enum`/`Closure` 转 `emitResourceContentsCleanup`。其他指针型转 `emitResourceContentsCleanup` + `emitLunaDeallocation` 释放。
- 谁调用：`emitCleanup`/`emitCanonicalCleanup`/`emitResourceContentsCleanup`/`getOrCreateDropCallback` 等。

**`void emitMaterializedIteratorCleanup(const string& name)`**
- 对物化迭代器的源数组，按 `sourceDropFlags` 逐元素检查是否仍被初始化，若已初始化则将其 `false` 标记并释放该元素。
- 谁调用：`emitIteratorPipeline`（exit 后）与 `emitCleanup`。

**`void emitCleanup(const string& place, CleanupAction action)`** / **`void emitCanonicalCleanup(const CleanupRecord& cleanup)`**
- `emitCleanup`：按名称在 `mLocals`/`mMaterializedIterators` 中查找，加载值，按 action（ResultDrop/EnumDrop/ArrayDrop/RecordDrop/DeviceRelease/Drop/Deallocate/None）分发。
- `emitCanonicalCleanup`：规范 CFG 版本——按 PlaceRef 投影（字段/index/deref）定位存储，再按 action 执行清理，支持 guarded 清理（按 cursor 判断元素是否越界）。
- 谁调用：`generateControlFlowBody` 的 cleanups 与 `free` 语句，以及 `generateTry` 的清理列表。谁被调：`emitOwnedPayloadCleanup`/`emitLunaDeallocation`。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的清理生成子模块。
- 依赖：`../core/TypeLayout.h`（布局/偏移）、`../core/TypeRelations.h`（typeId/typeRequiresCleanup）、`../runtime/RuntimeABI.h`（LUNA_DEFAULT_HOST_ALIGNMENT）、`CodeGenerator.h`、`CGHelpers.h`。
- 被调用方：`CodeGeneratorControlFlow.cpp`（emitCleanups 闭包、emitCanonicalFree）、`CodeGeneratorExpressions.cpp`（generateTry 的 cleanups）、`CodeGeneratorIterator.cpp`（emitIteratorPipeline 的 exit/cleanup）、`CodeGeneratorFunctions.cpp`（间接通过 ControlFlow）。

## 延伸阅读

1. `CodeGeneratorControlFlow.cpp`——规范 CFG 中如何引用 CleanupRecord。
2. `../core/TypeRelations.h`——`typeRequiresCleanup` 与 `typeId`。
3. `../runtime/RuntimeABI.h`——`LUNA_DEFAULT_HOST_ALIGNMENT`。

---

---
title: src/codegen/CodeGeneratorControlFlow.cpp
path: src/codegen/CodeGeneratorControlFlow.cpp
阶段: 代码生成 (CodeGen)——规范 CFG 主体 LLVM 生成
语言: C++
---
