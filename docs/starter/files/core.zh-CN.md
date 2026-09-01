> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/core/ —— 目录逐文件指南

本指南合并了 src/core/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: 编译期标准库符号契约
file: src/core/CoreContracts.h
namespace: luna::core_contracts
types: 无（仅常量）
阶段: 前端 SEMA / MoonIR 共用契约层
---

# src/core/CoreContracts.h

集中存放`编译器已知`的 Luna 标准库 trait 名、类型名与方法名的常量头文件。

## 这个文件做什么

把标准库留给编译器内部约定的身份字符串（trait 名、类型名、方法名）收拢到一处，
避免散落在 Sema、布局、所有权等代码里造成拼写漂移。仅含编译期常量，无逻辑、无运行期状态。

只记录 clean-break 0.3 的 canonical 命名，不保留 0.2 身份或语言模式分支。

## 关键结构体·类·枚举

本文件不含结构体/类/枚举，只有命名空间与 `inline constexpr const char*` 常量。

| 成员 | 含义 |
| --- | --- |
| `PackageId = "org.luna.core"` | 核心包身份标识。 |
| `canonical_0_3::*` | 0.3 新名（Drop、Clone、From、TryFrom、Option、Result、Iterator…）。 |
| `DropMethodName/CloneMethodName/...` | trait 方法名（drop、clone、next、into_iter、begin/push/finish）。 |
| `GlobalAllocatorDomainId` | 全局分配器域身份。 |

## 关键函数·方法

无函数。全部为 `inline constexpr const char*` 字符串常量。

## 与周边文件·阶段的关系

- 被 `src/core/SysMeta.h` 引用（`DropTraitId`、`OptionTypeId`、`IteratorTraitId` 等）。
- 阶段：前端 SEMA 资源决策与代码生成路径共用同一身份。

## 延伸阅读

- `src/core/SysMeta.h`：把原始常量包装成语义分组。
- `src/core/StableIdentity.h`：字符串身份如何压缩为稳定 hash。
- 类比 C 语言读者：相当于一组 `#define` 的`常量表`，只是用 `constexpr` 替代宏。


---

---
title: 所有权与使用度契约
file: src/core/Ownership.h
namespace: luna::ownership
types: Relation / Usage / Contract / CleanupAction
阶段: 前端 Sema（所有权推导）
---

# src/core/Ownership.h

区分`谁负责值`（所有权）与`责任可被消费多少次`（使用度）。

## 这个文件做什么

明确 `Ownership` 与 `Usage` 为两个正交维度：`move` 是状态迁移而非第三种类别。
提供类型化枚举、契约结构体与一元/二元判定函数。

## 关键结构体·类·枚举

| 类型 | 值 | 含义 |
| --- | --- | --- |
| `enum HeapStorageKind` | `Unique` | 堆存储方式。 |
| `enum Relation` | `Owned / SharedBorrow / MutableBorrow` | 责任归属。 |
| `enum Usage` | `Copy / Affine / Linear` | 消耗强度（0/1/2）。 |
| `struct Contract` | `relation` + `usage` | 所有权契约；提供 `==`/`!=`。 |
| `enum CleanupAction` | none/drop/deallocate/device_release/… | 生成的清理动作种类。 |

## 关键函数·方法

- `isBorrowed(Relation)`：非 Owned 即借用。
- `isMoveOnly(Usage)`、`mustConsume(Usage)`：move-only / 必须消费。
- `usageStrength(Usage)`：Copy=0、Affine=1、Linear=2。
- `satisfiesUsageRequirement`、`strongerUsage`：强度比较。
- `relationName`、`usageName`、`cleanupActionName`：枚举→字符串，用于诊断与稳定身份。

## 与周边文件·阶段的关系

- 被 `src/core/TypeSystem.h` 引用（`Contract`、`CleanupAction`、`Relation`/`Usage`）。
- `TypeRelations.cpp` 用 `relationName`/`usageName` 编码函数/闭包契约。
- `HeapStorageKind` 与 SysMeta 的 `ResourceManagement`、`ReleaseDomain` 互补。
- 阶段：前端 SEMA 所有权推导 → 供 MoonIR 布局与清理生成。

## 延伸阅读

- `src/core/TypeSystem.h`：`ResourceContract` 与 `parameterContractFor` 用到这里的关系/强度。
- `src/core/TypeLayout.h`：清理动作最终由布局/ABI 落实。

> 类比 C++：`Relation` 近似 Rust 借用/所有权；`Usage` 把`可复制/可移动/仅一次`数值化以作比较。


---

---
title: 稳定的身份标识
file: src/core/StableIdentity.h
namespace: luna::identity
types: StableId<Tag> 及前缀化别名
阶段: 前端身份分配
---

# src/core/StableIdentity.h

为编译器产物（类型、形状、符号、契约、ABI 布局）定义“稳定、与进程无关”的字符串身份。

## 这个文件做什么

提供带类型标签的 `StableId<Tag>` 包装，把“规范普通子串”压缩成稳定的 64 位 FNV-1a 十六进制身份。稳定身份可跨进程复用，是 MoonIR/容器校验的基础。字符串原文与压缩 ID 分离，hash 只作紧凑键。

## 关键结构体·类·枚举

| 类型 | 说明 |
| --- | --- |
| `template<typename Tag> struct StableId` | 存 `std::string value`，提供 empty/`==`/`!=`。 |
| 标签 `TypeIdTag/ShapeIdTag/SymbolIdTag/ContractIdTag/AbiLayoutIdTag` | 用空标签做类型区分（phantom type）。 |
| 别名 | `TypeId`/`ShapeId`/`SymbolId`/`ContractId`/`AbiLayoutId`。 |

## 关键函数·方法

- `stableIdentityHash(canonical)`：FNV-1a 64 位。
- `compactIdentity<Id>(prefix, canonical)`：前缀 + 16 位十六进制 hash。
- `symbolIdFromCanonical`/`contractIdFromCanonical`/`abiLayoutIdFromCanonical`：prefix 分别为 symbol_/contract_/abi_。

## 与周边文件·阶段的关系

- 被 `src/core/TypeIdentity.h` 与 `src/core/TypeRelations.cpp` 使用。
- `SysMeta.h` 的 `Facts.identity` 装载这些 ID。
- 阶段：身份由前端从 canonical 推导，后端 ABI 校验复用。

## 类比 C++

�StableId<Tag>` 类似用模板 + 空标签把若干 `using` 别名区分成不同类型的安全 ID。


---


---
title: 编译器权威的元数据（sysmeta）
file: src/core/SysMeta.h
namespace: luna::sysmeta
types: Facts / IdentityFacts / ControlFacts / ResourceFacts / CapabilityFacts / AbiFacts 及枚举
阶段: 前端派生，后端消费
---

# src/core/SysMeta.h

定义编译器“官方权威”的语义元数据：源码可读取其稳定投影，但永远不能构造、挂载或覆盖这些事实。

## 这个文件做什么

把类型、控制流、资源、能力、ABI 五类语义事实整理成类型化结构（`Facts`），让安全决策不依赖用户可控字符串。同时提供一组命名枚举的字符串输出函数。

## 关键结构体·类·枚举

| 类型 | 字段/值要点 |
| --- | --- |
| `enum ControlForm` | plain/interceptor/context/coroutine。 |
| `enum Cardinality` | none/once/many。 |
| `enum ContinuationStorage` | none/scoped_stack/persistent_frame。 |
| `enum Forwarding` | none/automatic/explicit。 |
| `enum ResourceManagement` | value/unique。 |
| `enum ReleaseDomain` | luna_global/foreign/device/executable/host_service。 |
| `enum ResourceLifetime` | value/lexical/borrowed/explicit。 |
| `struct IdentityFacts` | type/shape/symbol/contract/abiLayout 这些 `identity::*` ID。 |
| `struct ControlFacts` | form/cardinality/storage/forwarding/abortPermitted/replayValidated。 |
| `struct ResourceFacts` | parameters/result/management/releaseDomain/lifetime/relation/usage/cleanup/needsDrop… |
| `struct CapabilityFacts` | hostOnly/runtimeRetained/dynamicDispatch/ffi/gpu/maySuspend。 |
| `struct AbiFacts` | stableBoundary/persistentFrameRequired/dropGlueSymbol。 |
| `struct Facts` | schemaMajor/Minor + 五个子结构。 |

常量：`SchemaMajor=1`、`SchemaMinor=3`，`DropTraitId`/`DropMethodName`/`FromTraitId`/`ResultTypeId` 等。

## 关键函数·方法
`controlFormName`、`cardinalityName`、`continuationStorageName`、`releaseDomainName`、`resourceLifetimeName`、`forwardingName`、`resourceManagementName`：枚举名 → 稳定字符串。

## 与周边文件·阶段的关系
- 依赖 `CoreContracts.h`、`Ownership.h`、`StableIdentity.h`。
- 被 `TypeSystem.h` 引用，`Type` 持有 `sysmeta::Facts`；`TypeLayout` 与 ABI 消费这些事实。
- 阶段：Sema 派生 → MoonIR 后端。

## 延伸阅读
- `src/core/TypeSystem.h` 的 `makeSlot/makeFragment` 写成的 control facts。
- `docs/design/sysmeta.md`（如存在）。

> 类比 C++：`sysmeta` 相当于“编译器算出的 const 元信息表”，用户代码只读不可写。


---


---
title: 类型域与身份模式
file: src/core/TypeIdentity.h
namespace: luna::types
types: TypeDomain / IdentityMode / TypePtr
阶段: 前端类型语义起点
---

# src/core/TypeIdentity.h

定义类型所属的“域”（domain）与“身份模式”（identity mode），并引入 `Type` 前置声明与 `TypePtr`。

## 这个文件做什么

在尚未完整定义 `Type` 之前先划定类型分类框架：
- `TypeDomain` 说明类型在语言/编译器语义中的位置；
- `IdentityMode` 说明该类型用何种方式获得身份（结构/名义/内建…）。
同时用 `using` 把 `identity::TypeId`/`ShapeId` 引进来。

## 关键结构体·类·枚举

| 类型 | 值 | 含义 |
| --- | --- | --- |
| `template`—`struct Type`（前置声明） | — | 真正的类型节点定义见 `TypeSystem.h`。 |
| `using TypePtr = std::shared_ptr<Type>` | — | 类型节点统一用共享指针持有。 |
| `enum class TypeDomain` | value/meta/compiler/inference/error | 域。 |
| `enum class IdentityMode` | structural/nominal/builtin/meta_schema/compiler_intrinsic/inference/error | 身份方式。 |
| `using luna::types::TypeId`/`ShapeId` | 从 `luna::identity` | 稳定 ID 别名。 |

## 关键函数·方法
- 无直接函数；仅类型别名与枚举常量。

## 与周边文件·阶段的关系
- `TypeIdentity.h` → 被 `TypeSystem.h` 与 `TypeRelations.h`（含 .cpp）引用。
- `TypeRelations.cpp` 的 `canonicalShapeImpl`/`canonicalIdentityImpl` 依据 `domain`/`identityMode` 分支生成 canonical。
- 阶段：类型构造起点，供 Sema 解析（resolveType）与身份推导使用。

## 延伸阅读
- `src/core/StableIdentity.h`：`TypeId`/`ShapeId` 的真身。
- `src/core/TypeSystem.h`：`TypeKind` 全表。
- 类比 C++：可视作“类型节点公共前缀 + 判别种类”的声明骨架。


---

---
title: 目标无关的值布局计算
file: src/core/TypeLayout.cpp
namespace: luna::layout
阶段: 布局计算（前端推导 + 后端校验）
---

# src/core/TypeLayout.cpp

TypeLayout.h 的实现，按 TypeKind 逐步计算尺寸/对齐/字段偏移。

## 这个文件做什么

把 TypeLayout.h 的 API 落地：
- 用 alignTo 做对齐；
- 按 TypeKind 分派 valueSizeImpl/valueAlignmentImpl；
- 用递归遍历（带 active: unordered_set<const Type*> 防环）处理 Record/Closure/Enum 等可能自引用的复合类型；
- 生成 inline-ADT 布局签名。

## 关键结构体·类·枚举
- 无新公开类；有匿名命名空间（namespace {}）内部实现 valueSizeImpl、valueAlignmentImpl、variantPayloadSizeImpl，以及 active 环检测集。

## 关键函数·方法
公开（等同 TypeLayout.h 声明）：
- alignTo：对齐上取整。
- valueSize/valueAlignment：入口封装（新建空 active）。
- productStorageSize/productStorageAlignment/productFieldOffset：名义 Struct 堆载荷。
- variantFieldOffset/variantPayloadSize/enumPayloadSize：枚举/Result 变体。
- inlineAdtLayoutSignature：生成 luna.inline-adt.v1;tag_storage=8;payload_align=8;size=…;variant=… 签名。

## 与周边文件·阶段的关系
- 依赖 TypeLayout.h 与 TypeSystem.h（TypeKind/Type/TypeVariant）。
- 供后端与容器校验读取布局；与 TypeRelations.cpp 的“形状/身份”互补。

## 延伸阅读
- src/core/TypeLayout.h：接口与常量。
- src/core/TypeRelations.cpp：类型关系与身份。
- 类比 C++：相当于对结构体/枚举手动推导 sizeof+offsetof 的规约实现。

---


---
title: 值布局与 ABI 尺寸（头文件）
file: src/core/TypeLayout.h
namespace: luna::layout
types: 常量 + 尺寸/偏移函数
阶段: 布局计算（前端/MoonIR）
---

# src/core/TypeLayout.h

声明 Luna 值在编译器/MoonIR ABI 下的尺寸、对齐与字段偏移计算接口。

## 这个文件做什么

提供“目标无关”的布局查询：值大小、对齐、字段偏移、ADT（枚举/Result）的 inline 表示尺寸，以及一个可复现的 inline-ADT 布局签名。它是后续 `LayoutEngine` 的目标无关前身。

## 关键结构体·类·枚举

无自定义类；仅常量与函数签名：

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `InlineAdtAbiVersion` | 1 | inline ADT ABI 版本。 |
| `InlineTagStorageSize` | 8 | 判别 tag 存储字节。 |
| `InlinePayloadAlignment` | 8 | 载荷对齐字节。 |

## 关键函数·方法
- `alignTo(value, alignment)`：对齐上取整。
- `valueSize` / `valueAlignment`：ABI 值大小/对齐。
- `productStorageSize`/`productStorageAlignment`/`productFieldOffset`：指针化名义乘积类型的堆载荷布局。
- `variantFieldOffset`/`variantPayloadSize`/`enumPayloadSize`：枚举/Result 变体布局。
- `inlineAdtLayoutSignature`：可散列签名，供诊断/文档/Moon 容器兼容检查。

## 与周边文件·阶段的关系
- `#include "TypeSystem.h"`，依赖 `TypeKind`/`Type`/`TypeVariant`。
- 实现见 `TypeLayout.cpp`。
- 阶段：后端布局生成 + ABI 校验，独立于具体目标架构。

## 延伸阅读
- `src/core/TypeLayout.cpp`：实现细节。
- `src/core/TypeRelations.cpp`：形状/身份关联层，与布局互补。
- 类比 C++：近似 `sizeof`/`alignof` + `offsetof` 的建模，只是语义化、目标无关。


---

---
title: 规范身份的实现与类型关系判定
file: src/core/TypeRelations.cpp
namespace: luna::types
阶段: 类型规范生成与关系判定
---

# src/core/TypeRelations.cpp

TypeRelations.h 的实现：把类型图编码成规范化字符串，再折叠为稳定 ID，并提供关系谓词。

## 这个文件做什么

- 用 appendPart 做长度前缀分隔化编码（size:content;），保证规范串无歧义可解析。
- 用 stableHash 实现与 StableIdentity.h 一致的 FNV-1a 64 位哈希。
- canonicalShapeImpl 遍历结构（带 active/anchor 处理递归），canonicalIdentityImpl 依 identityMode 区分名义/内建/结构身份。
- 提供 sameType/sameShape/isAssignable/isExplicitlyConvertible/isAbiCompatible/isRecursiveShape。

## 关键结构体·类·枚举
匿名命名空间中的核心：appendPart、stableHash、compactId、domainName、kindName、canonicalShapeImpl、canonicalIdentityImpl、containsType。

## 关键函数·方法
- appendPart(output, part)：长度前缀编码。
- stableHash(value)+compactId：FNV-1a 64 位哈希并前缀化（shape_/type_）。
- canonicalShapeImpl/canonicalShape：结构型规约。
- canonicalIdentityImpl/canonicalType：依 identityMode 区分名义/结构/内建。
- shapeId/typeId 及 FromCanonical：稳定 ID 折叠。
- sameType/sameShape；isAssignable（sameType）；isExplicitlyConvertible(域检查+sameShape)；isAbiCompatible(保守同形状)；isRecursiveShape(containsType 自包含)。

## 与周边文件·阶段的关系
- 依赖 TypeRelations.h、TypeSystem.h（类型结构）与 Ownership.h。
- 与 StableIdentity.h 的 hash 保持一致。
- 阶段：类型推导与稳定标识生成。

## 延伸阅读
- src/core/StableIdentity.h：身份容器。
- src/core/TypeRelations.h：声明层。
- 类比 C++：对类型做规范化序列化 + 内容哈希以支持高速相等与可交换校验。

---

---
title: 类型的规范身份与关系
file: src/core/TypeRelations.h
namespace: luna::types
types: 身份查询 + 关系谓词
阶段: 类型规范/身份推导
---

# src/core/TypeRelations.h

声明类型的规范形式（canonical）生成、稳定 ID 计算与类型间关系谓词。

## 这个文件做什么

定义三层能力：
1. 从类型节点生成规范字符串（canonicalShape/canonicalType）；
2. 把规范字符串折叠为紧凑 ID（shapeId/typeId/shapeIdFromCanonical/typeIdFromCanonical）；
3. 类型关系谓词（sameType/sameShape/isAssignable/isExplicitlyConvertible/isAbiCompatible/isRecursiveShape）。

关键约定：canonical 有效载荷与紧凑 ID 分开保留——未来 Moon 容器校验必须比对/重算载荷，不能只信 hash。

## 关键结构体·类·枚举
无自定义类/枚举；全部是自由函数。

## 关键函数·方法
- canonicalShape(const TypePtr&)/canonicalType：结构/身份规范串。
- shapeId/typeId/shapeIdFromCanonical/typeIdFromCanonical：Compact ID（前缀 shape_/type_）。
- sameType/sameShape：等价判定。
- isAssignable/isExplicitlyConvertible：赋值/显式转换。
- isAbiCompatible：保守的目标无关 ABI 兼容（同形状）。
- isRecursiveShape：是否含递归形状。

## 与周边文件·阶段的关系
- 依赖 TypeIdentity.h（TypePtr/Domain/IdentityMode）；实现见 src/core/TypeRelations.cpp。
- 阶段：前端为每个类型推导规范串、拿到稳定 ID，供跨阶段比较/校验。

## 延伸阅读
- src/core/TypeRelations.cpp：实现（含 stableHash、分隔化编码）。
- src/core/StableIdentity.h：ID 的容器与 hash。
- 类比 C++：相当于对类型做规范化字符串序列化 + 内容哈希以支持相等/缓存。

---

---
title: 权威的目标无关类型系统模型
file: src/core/TypeSystem.h
namespace: 全局枚举 + luna::ownership + luna::sysmeta + luna::types
types: Type / TypeKind / TypeField / TypeVariant / ResourceContract / IteratorMode / IteratorOp / ContinuationKind
阶段: 前端与 MoonIR 共享的核心类型模型
---

# src/core/TypeSystem.h

Luna 前端的权威、目标无关类型模型：所有类型节点与分类、默认资源契约在此定义，是前端与后端共同依赖的核心。

## 这个文件做什么

定义编译器内部核心数据结构 Type 与其枚举分类，承载所有权与 sysmeta 资源事实，并提供一整套类型工厂、清理/契约判定与展示辅助。它是本文档族里其余文件（布局、关系、契约）的根基。

## 关键结构体·类·枚举

- 枚举 TypeKind：I8/I16/I32/I64/U8/U16/U32/U64/USize/ISize/F32/F64/Bool/String/CStr/RawPointer/Unit/Never/Struct/Record/Enum/Result/Trait/TypeParam/Reference/Function/Closure/Slot/Fragment/Iterator/DeviceBuffer/Event/Array/Slice/Metadata/MetadataView/DeclarationView/DeclarationRef/InferenceVar/Unknown。
- 枚举 ContinuationKind：Interceptor/Context。
- 枚举 IteratorMode：Copy/Shared/Mutable/Consuming/Range。
- 枚举 IteratorOp：None/Iter/IterMut/IntoIter/Range/Map/Filter/Take/Fold/ForEach/Count/Collect。
- struct TypeField{name,type}；struct TypeVariant{name,fields}。
- struct ResourceContract：relation/usage/cleanup/management/releaseDomain/lifetime/cleanupRequired/recursiveCleanup。
- struct Type：kind/domain/identityMode/name/declarationLinkageName/nominalId/typeParams/typeArgs/inner/arrayLength/isMutable/paramTypes/returnType/paramContracts/returnContract/capturedFields/sysmeta/isMultiShot/continuationKind/iteratorMode/fields/variants/inferenceId；含 isHeapType、toString 与全部 make* 工厂。
- 内置常用类型：TyI32..TyNever、TyEvent、TyUnknown（文件尾部的 inline 全局）。

## 关键函数·方法
- 工厂：makePrimitive、makeStruct、makeRecord、makeEnum、makeResult、makeIterator、makeTrait、makeTypeParam、makeReference、makeRawPointer、makeDeviceBuffer、makeArray、makeSlice、makeEvent、makeMetadata、makeMetadataView、makeDeclarationView、makeDeclarationRef、makeFunction、makeClosure、makeSlot、makeFragment、makeUnknown、makeInferenceVar。
- 查询/契约：isHeapType、defaultUsageForType、typeRequiresCleanup、typeHasRecursiveCleanup、cleanupActionForType、resourceContractForType、parameterContractFor、isNumericType、isIntegerType。
- 解析：resolveType(TypeAST*, typeBindings) 把类型 AST 解析成规范化 Type。
- 展示：Type::toString()（人类可读的类型串）。

## 与周边文件·阶段的关系
- 依赖：TypeIdentity.h、Ownership.h、SysMeta.h。
- 被：TypeLayout.*、TypeRelations.* 及整个 frontend/MoonIR 使用。
- 阶段：前端类型构造与 Sema 派生 → 后端布局/ABI。

## 延伸阅读
- src/core/TypeRelations.*：规范/身份。
- src/core/TypeLayout.*：布局/ABI 尺寸。
- src/core/SysMeta.h：facts 定义。
- 类比 C++：一个带判别式的类型节点 + 资源契约，类似多工厂的 type system 类集合。


---
