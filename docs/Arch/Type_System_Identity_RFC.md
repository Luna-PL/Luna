# Luna 类型域、结构形状与身份 RFC

> 状态：Accepted / Core implemented v0.2（2026-07-22）  
> 范围：Value Type、Meta Type、Compiler Type、Structural/Nominal Identity、Type Relations、MoonIR Type Table

## 1. 核心原则

Luna 必须分开以下概念：

- 值的语义类型；
- 类型的结构形状；
- 显式名义身份；
- 声明身份；
- Metadata schema 与 attachment；
- 目标相关物理布局与 ABI。

Metadata attachment 不进入普通 `TypeId`。类型名不得被当作结构类型的身份。物理布局不得反向决定语言类型相等性。

## 2. 三个类型域

### 2.1 Value Type

参与正常执行的 primitive、product/record、sum/enum、callable、reference/pointer、array/slice、slot/fragment 和 nominal wrapper。

### 2.2 Meta Type

`meta` schema 声明的编译期值类型。Schema 始终以 `MetaSchemaId` 名义化；字段相同的两个 schema 不是同一 Meta Type。

Retention 是 Metadata 实例的保留属性，不改变 Meta Type。`runtime@...` 必须额外满足可验证的 runtime encoding 约束。

### 2.3 Compiler Type

`declaration_view<T>`、`declaration_ref<T>`、未来的 `type_token<T>` 等编译器协议类型。它们不是默认可导出的运行时值。

Inference Variable 和 Error Type 是 Sema 内部状态，不属于已定型 MoonIR。

## 3. Shape 与 Identity

```text
SemanticType
  TypeDomain
  TypeForm
  ShapeId
  IdentityMode
  NominalDeclarationId?
  GenericArguments[]
  SemanticQualifiers
```

`ShapeId` 由规范化的结构生成，不包含源码显示名、Metadata、Retention 或源码位置。第一版 product/sum 形状对字段/变体顺序敏感，与当前零拷贝内存降级保持一致；宽度子类型和重排转换留待独立 RFC。

```text
structural TypeId = H(domain, form, ShapeId, semantic qualifiers)
nominal TypeId    = H(domain, NominalDeclarationId, generic TypeIds)
meta TypeId       = H(meta, MetaSchemaId)
```

Hash 只是索引。MoonIR/MoonRuntime 必须保留并核验规范化 payload，不得仅信任 hash。

## 4. 源码语义

```luna
struct Point { x: i32; y: i32; }          // 默认结构类型
nominal struct UserId { value: i64; }      // 显式名义类型
nominal struct OrderId { value: i64; }
```

`UserId` 和 `OrderId` 形状可相同，但类型不相同、不可隐式赋值。`nominal` 是编译器语义修饰符，不是普通用户 Metadata。

Struct 和 Enum 默认结构化。Trait 与 Meta Schema 始终名义化。FFI/opaque/递归类型的更强限制将基于安全边界逐步增加；结构递归在第一版禁止，并提示改用 `nominal`。

## 5. 类型关系

必须以显式 API 区分：

```text
sameType
sameShape
isAssignable
isExplicitlyConvertible
isAbiCompatible
isRuntimeSubstitutable
```

- 结构类型的 `sameType` 比较 `ShapeId`。
- 名义类型的 `sameType` 比较 Nominal ID 和泛型实参。
- `sameShape` 忽略 nominal brand，但不授予隐式赋值权。
- `isAssignable` 第一版只接受 `sameType`。
- ABI 兼容由目标相关 Layout Engine 判定，不代表源语言可赋值。
- Dynamic replacement 需要额外的 callable、ownership、capability 和 runtime descriptor 合约。

## 6. Metadata 边界

Metadata attachment 属于 Declaration，不属于共享的结构 `TypeId`。两个结构相同的声明可共享类型，但不会互相继承 Metadata。

Metadata 角色后续分为 `Annotation` 和 `Discriminator`。只有 Discriminator 可参与 DeclarationId/候选选择；两者都不参与 TypeId/ShapeId/CallableType/LayoutId。角色语法在独立 Metadata RFC 冻结前，当前 attachment 保持已有 selector 行为。

## 7. MoonIR 边界

MoonIR 最终将使用不可变表：

```text
type_table
shape_table
nominal_declaration_table
metadata_schema_table
metadata_attachment_table
```

MoonIR 不保留 Inference Variable/Error Type。LLVM 后端由 Shape/ABI 计算 Layout，同时保留 TypeId 用于语言级验证。

## 8. 迁移阶段

1. 引入 TypeDomain、IdentityMode、TypeId、ShapeId 和 TypeRelations，保留旧 `TypePtr` 外壳。
2. 引入 `nominal struct/enum`，将默认 struct/enum 切换为结构身份。
3. Trait/impl、泛型实例和反射改用稳定 TypeId，移除 `toString()` 身份。
4. 将 ConstraintSolver/Inference 移出 Core Type，Moon verifier 只接受已定型类型。
5. MoonIR lowering 结束时封存 Type/Shape table，后端只能读取已验证的冻结快照。
6. 引入目标相关 LayoutId/ABI relation 与容器验证。

## 9. 实现状态

已完成：

- `TypeDomain`、`IdentityMode`、`TypeId`、`ShapeId` 和规范 payload；
- 显式 `sameType/sameShape/isAssignable/isExplicitlyConvertible/isAbiCompatible` 关系层，已移除内部 `Type::equals()` 调用；
- `struct/enum` 默认结构身份与 `nominal struct/enum` 语法；
- Trait、Meta Schema 和名义 Value Type 使用 package/module-qualified 声明身份；
- 结构递归禁止与精确字段/变体顺序关系；
- Inference Solver 从 Core Type 物理移入 Sema；
- MoonIR `type_table`/Shape payload 和 verifier 重算校验；
- MoonIR type table 在 lowering 结束时确定性排序并封存；每个记录保存直接引用的 `TypeId`，封存后禁止继续注册类型；
- callable shape 包含参数/返回值的 ownership relation 与 Copy/Affine/Linear contract；
- 编译期 `type_id/type_shape/type_domain/type_same/type_same_shape/type_abi_compatible` 反射；
- Trait impl 索引和泛型实例 key 改用稳定 TypeId。

延期项：

- 前端完全不可变、interned TypeArena；当前冻结边界已经位于 MoonIR type table，操作节点上的过渡 `TypePtr` 只能引用该表中已验证的类型且不能触发封存后注册；
- 目标相关 LayoutEngine/`LayoutId` 和 C/Plugin ABI 精确关系；当前 ABI 关系是仅接受 Value `sameShape` 的保守前置实现；
- `type`/`nominal type` 别名语法和 opaque type；
- Metadata `Annotation/Discriminator` 角色语法；
- 结构递归、宽度子类型、字段重排和显式转换。
