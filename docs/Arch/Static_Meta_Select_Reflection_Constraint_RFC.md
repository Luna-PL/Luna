# Static Meta、Select、Reflection 与 Constraint 边界 RFC

> 状态：静态首版已实现  
> 范围：只冻结编译期模型；不冻结 `runtime` / `dynamic` 权利集合

## 1. 目标

本 RFC 划分四个容易被混用、但职责不同的机制：

- Metadata：给语言对象附带用户定义的信息；
- Select：在开放候选边界中执行用户定义的选择策略；
- Reflection：观察已经确定的语言对象；
- Constraint：声明由编译器验证的安全命题。

共同原则是按需付费。没有显式 `runtime` 或 `dynamic` 时，metadata 查询、
selector 执行、声明反射和 constraint 求解都在编译期完成，并在进入 MoonIR
前擦除。

## 2. Metadata

### 2.1 职责

`meta` 声明具名、类型化的附带信息：

```luna
meta release {
    channel: string;
    major: i32;
}

@release("stable", 3)
fn answer() -> i32 { return 30; }
```

编译器验证：

- schema 存在；
- 字段数量和字段类型正确；
- 静态 attachment 参数可在编译期求值；
- schema identity 与 attachment identity 稳定。

编译器不验证：

- `major` 是否真的是版本号；
- `channel` 的业务含义；
- 哪个版本更新、更稳定或更适合调用；
- 任意用户约定的 metadata 之间是否语义一致。

因此 Metadata 是信息承载机制，不是安全约束，也不是选择策略。

### 2.2 Metadata 不改变 callable type

以下声明具有相同的 callable type，但不同的 declaration identity：

```text
CallableType  = 如何调用
DeclarationId = 调用哪个声明
Metadata      = 声明携带什么附带信息
```

Metadata 不参与普通结构类型等价。是否使用 metadata 区分候选，由声明解析和
selector 边界决定。

## 3. Static Reflection

Reflection 是基础机制。它观察一个已经确定的对象，不要求对象携带 Metadata，
也不要求经过 Select。

当名字和可选签名足以确定声明时：

```luna
let known = declaration_of::<(i32) -> i32>(answer);
let id = declaration_id(known);
let signature = declaration_signature(known);
```

`declaration_of` 返回静态 `declaration_ref<T>`。该值可以交给静态反射查询，
但不会自动产生 Runtime Descriptor。

若名字和签名仍对应多个声明，`declaration_of` 必须报告歧义。它不能暗中按源码
顺序、Metadata 或“最新版本”选择。此时边界是开放的，应使用 Select。

类型反射与声明反射相互独立：

- `type_id::<T>()`、`type_size::<T>()` 等处理类型；
- `declaration_id(ref)`、`declaration_signature(ref)` 等处理声明；
- Metadata 查询是声明反射可观察的一类附带信息，不是反射存在的前提。

## 4. Static Select

### 4.1 适用场景

Select 用于用户无法在源码中写出一个唯一确定对象、但可以对有限候选集合执行
策略的场景。典型情况是同一声明族可以由其他模块继续增加候选，或者多个候选
具有相同 callable type。

如果对象已经由普通名称和签名唯一确定，不应强制使用 Metadata 或 Select。

### 4.2 公开调用协议

Selector 声明侧必须显式写出候选视图参数：

```luna
fn choose(
    candidates: declaration_view<() -> i32>,
    wanted: i32
) -> declaration_ref<() -> i32> {
    // user policy
}
```

调用侧不显式构造候选视图：

```luna
let selected = select answer with choose(3);
```

`select` 是候选视图的唯一合法构造边界。它根据目标声明族、可见性和 callable
shape 构造有限视图，然后按公开协议把视图作为 selector 的第一个参数。

这不是隐藏 AST 魔法：`declaration_view<T>` 是真实、只读、可遍历的内置类型，
selector 函数体按普通控制流语义执行。

### 4.3 内置静态类型

```text
declaration_view<T>  有限只读候选集合
declaration_ref<T>   带稳定 DeclarationId 的声明引用
metadata_view<M>     某声明上 schema M 的有限只读实例集合
```

首版操作：

```luna
for candidate in candidates { ... }
declaration_count(candidates)
declaration_at(candidates, index)

for value in metadata::<release>(candidate) { ... }
declaration_has_metadata::<release>(candidate)

declaration_id(candidate)
declaration_signature(candidate)
```

Metadata 实例是 schema 的正常类型化值，因此可以通过字段访问和普通表达式进行
比较：

```luna
for info in metadata::<release>(candidate) {
    if info.channel == wanted && info.major > best_major {
        best = candidate;
        best_major = info.major;
    }
}
```

### 4.4 编译器与用户的责任

用户负责：

- 遍历、过滤、排序和打分策略；
- Metadata 的业务解释；
- 在函数控制流中返回一个 `declaration_ref<T>`。

编译器负责：

- 构造有限、同族、签名兼容的候选视图；
- 编译期执行 selector；
- 验证返回值属于输入视图；
- 将结果固化为具体 `DeclarationId` 和 linkage symbol；
- 报告无返回、越界访问、不可编译期求值或越界引用。

编译器不要求 selector 调用 `select_unique`。该操作只是保留的精确 Metadata
匹配便利原语。

### 4.5 静态成本

静态 Select 的结果在 MoonIR 中是普通的已解析实体引用。以下内容不会因为静态
Select 自动进入产物：

- 候选视图；
- selector 机器码；
- compile-time-only Metadata；
- 通用声明 Registry；
- Runtime Descriptor。

## 5. Constraint

### 5.1 与内部类型求解器分离

编译器内部的类型统一、数值推断和错误恢复不是用户 Constraint。用户可见的
Constraint 是具名编译期布尔命题：

```luna
constraint SmallValue<T> =
    type_size::<T>() <= 8;

constraint PlainSmallValue<T> =
    !type_is_meta::<T>() && SmallValue::<T>();
```

使用点：

```luna
fn keep<T>(value: T) -> T where PlainSmallValue<T> {
    return value;
}
```

### 5.2 首版求解规则

- constraint 至少有一个类型参数；
- predicate 必须是 `bool`；
- 可使用静态类型反射、常量表达式和其他 constraint；
- 支持普通布尔组合与比较；
- 在泛型实例化点代入具体类型；
- `false` 表示约束不满足；
- 不能静态求值表示编译错误；
- 递归 constraint 组合不能形成求值环；
- 求解结果不进入 MoonIR，不生成 Runtime 检查。

Trait bound 与 Constraint 保持不同语法和语义：

```luna
where T: Sequence       // 已实现某种行为能力
where SmallValue<T>     // 编译器可证明某个命题
```

首版不包含 C++ `requires` expression。即暂时不能用语法块探测任意表达式是否
well-formed；该能力需要独立的“仅类型检查、不生成代码”语义阶段。

## 6. 四者关系

| 机制 | 输入已唯一确定？ | 编译器理解业务语义？ | 产生安全判定？ | 默认运行时成本 |
| --- | --- | --- | --- | --- |
| Metadata | 无要求 | 否 | 否 | 无 |
| Reflection | 是 | 只理解语言结构 | 否 | 无 |
| Select | 否，输入是候选集合 | 否，策略属于用户 | 只验证返回引用合法 | 无 |
| Constraint | 在实例化点确定类型参数 | 理解声明的可证明命题 | 是 | 无 |

组合是允许的，但不是强制的：

- Select 可以读取 Metadata，也可以只读取声明签名；
- Reflection 可以读取 Metadata，也可以完全不涉及 Metadata；
- Constraint 可以使用类型反射，未来也可定义 Metadata 命题；
- 已知声明可以直接 Reflection，不需要 Select；
- Metadata 可以只服务文档或工具，不参与 Select/Constraint。

## 7. Runtime 与 Dynamic 暂不冻结

本 RFC 不从静态能力反推出 Runtime/Dynamic 权利。尤其不能假定：

- 静态可遍历视图必然具有相同的 runtime encoding；
- 静态反射查询默认被保留到运行时；
- `runtime` 自动拥有完整反射或替换能力；
- `dynamic` 的最终权利集合已经由现有实验实现冻结。

当前唯一冻结原则是显式付费：

```text
未写 runtime/dynamic
    => 静态求值并擦除

显式 runtime/dynamic
    => 只为最终规范授予的能力生成表示和成本
```

广义 dynamic selector、runtime reflection、注册、替换、失效与生命周期规则需在
后续 RFC 中单独讨论。
