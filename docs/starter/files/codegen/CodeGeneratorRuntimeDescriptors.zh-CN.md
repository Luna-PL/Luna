# src/codegen/CodeGeneratorRuntimeDescriptors.cpp —— 运行时声明描述符与注册表的 LLVM IR 生成

## 这个文件做什么

实现 `CodeGenerator::emitRuntimeDescriptors()`——遍历 `mProgram->declarationTable`，为每个运行时可见的声明生成 LLVM 全局常量的「声明描述符」（`moon.declaration.descriptor` 结构），包含声明 ID、族 ID、linkage 名、种类、保留期、元数据数组与入口函数指针，并在特定段（Mach-O/COFF/ELF 各不同）中放置描述符数组与注册表，最后通过 `llvm::appendToCompilerUsed` 防止 GC 丢弃。当 `mProgram->features.runtime` 为 false 时直接返回。

对 C++ 读者：这是「运行时反射元数据」的生成器。它把 Luna 编译期已知的声明清单转成 C 兼容的全局结构体，嵌入到 ELF `.moon.runtime.descriptor` 等专用段，供未来可能的 MoonRuntime 加载器枚举。技术的本质是「编译器生成的自描述段」。

## 关键结构体·类·枚举

匿名命名空间内：
- `struct MoonRuntimeSectionNames`：含 `descriptors` 与 `registry` 两个 `const char*`，用于按目标格式选择段名。
- `MoonRuntimeSectionNames moonRuntimeSectionNames()`：根据 `llvm::Triple(getProcessTriple())` 判断格式：Mach-O 返回 `{"__DATA,__moon_desc", "__DATA,__moon_registry"}`；COFF 返回 `{".moon$D", ".moon$R"}`（$ 后缀是 COFF 子段规范写法）；默认 ELF 返回 `{".moon.runtime.descriptor", ".moon.runtime.registry"}`。
- `uint64_t stableRuntimeId(const string& text)`：FNV-1a 64 位哈希，生成稳定 ID 用于命名全局量。

LLVM 结构体类型（在 `emitRuntimeDescriptors` 中动态创建）：`moon.metadata.value`（`{i8, i64, ptr}`）、`moon.metadata.instance`（`{ptr, i64, ptr, i8}`）、`moon.declaration.descriptor`（`{i32, ptr, ptr, ptr, i8, i8, i64, ptr, ptr}`）。

## 关键函数·方法

**`void CodeGenerator::emitRuntimeDescriptors()`**
- 前提：`mProgram==nullptr || !mProgram->features.runtime` 则直接返回。
- 创建三个 LLVM 结构体类型（metadata value/instance、declaration descriptor）。
- 局部的 `cString(text)` 闭包：对每个字符串常量，创建 `GlobalVariable`（`ConstantDataArray::getString`，true 只读，PrivateLinkage，`__moon_string_<hash>` 命名，UnnamedAddr::Global），返回 GEP 到首字符的 i8* 常量表达式。用 `unordered_map` 缓存。
- 遍历 `mProgram->declarationTable`：对每个 `record`，过滤掉保留期为 `CompileTime` 且无运行时元数据的声明。保留元数据（`retention!=CompileTime`）展开为 `moon.metadata.instance` 数组，每个 instance 的 values 展为 `moon.metadata.value` 数组（区分 integer/float/boolean/string 四种 payload）。
- 构造 descriptor：`{version=1, id, familyId, linkageName, kind, retention, metadataCount, metadataPointer, entry}`，其中 `entry` 取 `mFunctions[linkageName]`（若存在）否则 null。
- 以 `__moon_descriptor_<hash>` 命名 InternalLinkage 全局常量，设 `setSection(runtimeSections.descriptors)`。
- 收集所有 descriptor 指针为 `descriptorPointers` 数组，构造注册表结构 `{count, [descriptorPointers]}`，以 `__moon_runtime_registry_<hash>` 命名 ExternalLinkage，设 `setSection(runtimeSections.registry)`。
- 最后 `llvm::appendToCompilerUsed(*mModule, retainedGlobals)` 防止 GC 丢弃。
- 谁调用：`CodeGeneratorModule.cpp` 的 `generate()` 在 Pass1 之后、Pass2 之前。谁被调：LLVM 的 Module/GlobalVariable/ConstantExpr 构造。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的元数据发射子模块，在 `generate()` 主流程的中间步骤调用。
- 依赖 `CodeGenerator.h`、`../moonir/MoonIR.h`（mProgram 的 declarationTable、MetadataInstance、Retention 枚举）、LLVM 的 ModuleUtils（`appendToCompilerUsed`）。
- 被 `CodeGeneratorModule.cpp` 调用；自身不调用其他 codegen 实现文件。

## 延伸阅读

1. `CodeGeneratorModule.cpp` 的 `generate()`——本函数在其中的位置。
2. `../moonir/MoonIR.h` 中的 `DeclarationRecord`、`MetadataInstance`、`Retention`。
3. LLVM 的 Section/GlobalVariable 文档，以及 `appendToCompilerUsed`。

---
