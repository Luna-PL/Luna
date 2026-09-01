# src/sema/SemanticAnalysisSupport.h — Sema 通用内联工具集

> 一句话定位：一组 header-only 的内联小工具：声明身份（identity）/链接名（linkage name）生成、类型替换、元数据键、拆分限定名等，供各分析组件复用。

## 这个文件做什么

语义分析多个组件都要处理「声明的稳定身份」「名字如何拼成链接名」「类型参数替换」这类事务。这个头文件把它们集中成 inline 函数（避免跨 TU 重复定义），没有 .cpp。

要点：

- **声明身份**：`nominalDeclarationIdentity` 生成 `package::module::kind::symbol` 形式的稳定身份（`kind` 为 `struct`/`enum`/`trait`/`meta`/`fn` 等）；`functionDeclarationIdentity` 使用 metadata 与规范化 callable source signature，不使用依赖重载数量的 executable linkage。
- **链接名**：`metadataDeclarationName` 给带元数据的声明生成 `base__meta_<hash>` 基础链接名；`sourceTypeIdentity`/`functionSourceSignatureIdentity` 规范化 callable source signature（含 type parameter alpha-normalization），`declarationSourceIdentity` 将签名与 metadata key 组合，`isolatedLinkageName` 再生成 `__luna_<hash>_<sourceName>`。
- **稳定哈希**：`stableMetadataHash` 是 FNV-1a 64 位哈希。
- **元数据键**：`metadataExpressionKey` 把常量表达式转成稳定字符串键。
- **名字工具**：`qualifiedDeclarationKey`/`splitQualifiedName`/`effectivePackageId`/`nominalTypeOwner`。
- **类型工具**：`substituteNominalType`（递归替换类型参数）、`reachesInlineType`（检测内联递归布局）。

C++ 类比：这相当于一组编译期工具函数库：名字修饰（name mangling）辅助 + 模板参数替换。

## 关键结构体·类·枚举

无类/枚举；全部是 inline 自由函数（除 `substituteNominalType` 外大多纯字符串处理）。

## 关键函数·方法

- `displayTraitRef(trait)`：返回 trait 名。
- `nominalDeclarationIdentity(program, kind, symbol, decl)`：拼 `owner(::module)::kind::symbol` 稳定身份。
- `stableMetadataHash(value)`：FNV-1a 64 位。
- `metadataExpressionKey(expr)`：常量表达式 → `i:`/`f:`/`b:`/`s:`/`id:`/`expr@` 前缀键。
- `metadataDeclarationName(base, decl)`：带元数据的声明链接名（`base__meta_<hash>`）。
- `sourceTypeIdentity(type, typeParameters)` / `functionSourceSignatureIdentity(function)`：在 resolved `TypeId` 尚不可用时生成长度分隔的规范化 source type/callable identity。
- `declarationSourceIdentity(base, decl)`：把 metadata 基础 linkage 与 function signature discriminator 组合；非 function 声明保持旧身份。
- `functionDeclarationIdentity(program, function)`：从 source discriminator 生成 function DeclarationId，在添加或移除其他 overload 时保持已有 SymbolId 稳定。
- `effectivePackageId(program, decl)`：声明/程序/`main` 的包 id 优先级。
- `nominalTypeOwner(type)`：从 `nominalId` 提取 `::` 前的 owner。
- `qualifiedDeclarationKey(packageId, modulePath, name)`：拼完整限定键。
- `splitQualifiedName(name)`：按 `::` 拆分。
- `reachesInlineType(current, target, active)`：检测 target 是否内联可达（array/Result/enum 递归；product 与指针/引用/shared 是表示层屏障）。
- `isolatedLinkageName(key, sourceName)`：`__luna_<hash>_<sourceName>`。
- `substituteNominalType(type, bindings)`：深拷贝 `Type` 并递归替换 `TypeParam`，保留数组/slice/闭包/契约/元数据视图/槽/片段等全部语义属性。

## 与周边文件·阶段的关系

- 被 `SemanticContext.cpp`、`DeclarationCollector.cpp`、`TypeResolver.cpp`、`BodyAnalyzer.cpp`、`CompileTimeEvaluator.cpp` 等 include 使用。
- 声明身份/链接名逻辑与 `SemanticContext::analyze` 里的链接名分配（`isolatedLinkageName`）配套。
- `substituteNominalType` 是特化（`TypeResolver::monomorphize`/`instantiateNominal`）与 trait 方法签名比对（`BodyAnalyzer::analyzeImpl`）的关键工具。

## 延伸阅读

- `TypeResolver.cpp`（`MonomorphizationCloner` 使用 `substituteNominalType`）。
- `SemanticContext.cpp`（链接名分配）。
- `DeclarationCollector.cpp`（身份/键生成）。


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticAnalyzer.cpp
lang: zh-CN
audience: 学过 C/C++、想读 Luna 语义分析装配代码的读者
---
