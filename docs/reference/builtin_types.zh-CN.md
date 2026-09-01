# Luna 0.3 开发期内置类型清单

> 文档类别：开发期参考与实现状态矩阵
> 适用版本：Luna 0.3.0 候选版
> 状态：活跃开发；各项稳定性见表格
> 规范性：源码拼写、类型域和 usage 规则规范；布局数字为 Internal Alpha ABI
> 首次实现核对：`d0ab31c`（2026-07-31）

本文是当前类型清点的权威列表。它不把 `TypeKind` 等同于用户可见类型，并明确区分
编译器内置、用户声明、标准库声明、Compiler recipe 和 Sema 内部状态。

## 1. 布局口径

下表中的大小/对齐来自当前 64 位编译器/MoonIR Alpha 布局引擎：

- 它不是所有目标平台的永久承诺；
- 它不是允许穿过 C ABI 的充分条件；
- `type_size::<T>()` 当前使用这一口径；
- `unit`/`never` 的值大小为 0，但对齐查询返回 1；
- pointer-represented 不表示可以与任意 C 指针互换。

## 2. 源码可见原子内置类型

| 拼写 | 域/身份 | 默认 usage | 当前大小/对齐 | 当前语义与状态 |
|---|---|---:|---:|---|
| `i8` | Value/Builtin | Copy | 1/1 | 有符号 8 位整数；Frozen for Alpha |
| `i16` | Value/Builtin | Copy | 2/2 | 有符号 16 位整数；Frozen for Alpha |
| `i32` | Value/Builtin | Copy | 4/4 | 有符号 32 位整数；整数默认字面量；Frozen for Alpha |
| `i64` | Value/Builtin | Copy | 8/8 | 有符号 64 位整数；Frozen for Alpha |
| `u8` | Value/Builtin | Copy | 1/1 | 无符号 8 位整数；Frozen for Alpha |
| `u16` | Value/Builtin | Copy | 2/2 | 无符号 16 位整数；Frozen for Alpha |
| `u32` | Value/Builtin | Copy | 4/4 | 无符号 32 位整数；Frozen for Alpha |
| `u64` | Value/Builtin | Copy | 8/8 | 无符号 64 位整数；Frozen for Alpha |
| `usize` | Value/Builtin | Copy | 8/8 | 当前 64 位无符号大小类型；非 64 位策略未冻结 |
| `isize` | Value/Builtin | Copy | 8/8 | 当前 64 位有符号大小类型；非 64 位策略未冻结 |
| `f32` | Value/Builtin | Copy | 4/4 | IEEE 后端浮点表面；Frozen for Alpha |
| `f64` | Value/Builtin | Copy | 8/8 | 浮点默认字面量；Frozen for Alpha |
| `bool` | Value/Builtin | Copy | 1/1 | 条件与逻辑类型；当前不开放为 C FFI 类型 |
| `string` | Value/Builtin | Affine | 8/8 | 拥有、pointer-represented 字符串；格式化 API 未冻结 |
| `cstr` | Value/Builtin | Copy | 8/8 | C 风格字符串指针边界；不拥有目标字节 |
| `unit` | Value/Builtin | Copy | 0/1 | 无有意义返回值；Frozen for Alpha |
| `never` | Value/Builtin | Copy | 0/1 | 不可构造 bottom type；Frozen for Alpha |
| `event` | Value/Builtin | Linear | 4/4 | launch 完成事件；必须 await/转移；异构表面 Experimental |

`event` 虽可被类型解析器识别，正常值来源是 `launch`；用户不能构造一个有效设备
事件常量。

## 3. 源码可见类型构造器

| 拼写 | 域/身份 | 形成规则 | 默认 usage | 当前表示/状态 |
|---|---|---|---|---|
| `raw<T>` | Value/Structural builtin constructor | 恰好一个 `T` | Copy，可由显式契约改为 Linear owner | 8 字节裸指针；FFI 支持 |
| `&T` | Value/Structural | 一个 `T` | Copy handle；SharedBorrow relation | 8 字节；loan 受检查 |
| `&mut T` | Value/Structural | 一个 `T` | Copy handle；MutableBorrow relation | 8 字节；独占 loan |
| `array<T, N>` | Value/Structural | 一个 `T` 和非负编译期整数 `N` | 由 `T` 决定 | 内联 `N * size(T)`；Frozen core |
| `slice<T>` | Value/Structural | 恰好一个 `T` | Copy handle + 来源 shared loan | 16 字节 `{data,length}`；当前只读 |
| `Result<T, E>` | Value/Canonical Core 名义声明 | 恰好两个载荷类型 | `join(usage(T), usage(E))` | `org.luna.core::result::Result`；inline ADT v1 |
| `device_buffer<T>` | Value/Structural builtin constructor | 恰好一个元素类型 | Linear | 8 字节句柄；当前操作只稳定支持 `i32` |
| `(P...) -> R` | Value/Structural | 参数序列和返回类型 | Copy function value；contract 属于 shape | 8 字节代码/闭包入口表示；closure env 未冻结 |
| `affine T` | 非独立类型 | 只用于 usage contract | Affine | TypeId 仍为 `T` |
| `linear T` | 非独立类型 | 只用于 usage contract | Linear | TypeId 仍为 `T` |

当前 `raw<T>` 不携带 allocator domain。只有外部声明的 `linear raw<T>` 返回契约表达
拥有义务；释放者匹配仍由 FFI 声明者负责。

## 4. 声明形成的类型

| 来源 | 域/身份 | 默认 usage | 当前表示/状态 |
|---|---|---|---|
| `struct` | Value/Nominal | Affine | pointer-represented product；声明身份不可擦除 |
| `enum` | Value/Nominal | 载荷 usage 上确界 | inline ADT v1 + 声明身份 |
| `trait` | Compiler/Nominal | 不作为普通运行时值 | 静态解析行为契约 |
| `meta` schema | Meta/MetaSchema | 编译期值 | 默认无普通运行时表示 |
| type parameter/`Self` | Compiler/CompilerIntrinsic | 由实例化类型决定 | MoonIR 前必须实例化或合法保留为模板事实 |
| slot type | Value/Structural control contract | 不作为普通 owning data；内部 handle 默认为 Copy | host-only、Once/Many 属于 shape |
| fragment type | Value/Structural control contract | 不作为普通 owning data；内部 handle 默认为 Copy | host-only、interceptor/context 属于 shape |

具名 product 的默认 Affine 目前来自其独占 heap 表示。不同具名 product 即使
`type_same_shape` 为真，TypeId 也始终不同。

## 5. 编译期可见的编译器内在类型

| 拼写/内部名称 | 域/身份 | 用户可写 | 运行时 | 状态 |
|---|---|---|---|---|
| `metadata_view<M>` | Compiler/CompilerIntrinsic | 是，必须一个 Meta schema 参数 | 默认擦除 | Implemented Experimental |
| `declaration_view<T>` | Compiler/CompilerIntrinsic | 是，0 或 1 个 callable 参数 | 默认擦除 | Implemented Experimental |
| `declaration_ref<T>` | Compiler/CompilerIntrinsic | 是，0 或 1 个 callable 参数 | 默认擦除 | Implemented Experimental |
| compiler Iterator recipe | Compiler/CompilerIntrinsic | 不能作为公开命名类型构造 | 不形成稳定 iterator ABI | Implemented Experimental |
| `{ x: T, y: U }` record | Value/Structural | 可写；不使用 `record` 关键字 | 内联、按字段名规范化 aggregate | 0.3 开发期已实现 |

`declaration_view` 是集合式静态选择视图；`declaration_ref` 是已解析单一声明引用。
两者都不是可以传给普通 FFI 或长期存储的反射对象。

## 6. 纯 Sema/MoonIR 前内部状态

| 内部项 | 域/身份 | 含义 | MoonIR |
|---|---|---|---|
| `InferenceVar` | Inference/Inference | 尚未求解的约束变量 | 必须拒绝 |
| `Unknown` | Error/Error | 诊断恢复占位 | 必须拒绝 |
| 源码 `auto` | 不是类型 | 请求创建 InferenceVar | 不直接出现 |

错误恢复时把缺失类型暂时写作 `i32` 只是为了继续报告更多诊断，不赋予错误程序
有效的 `i32` 语义。

## 7. 标准库声明类型

以下类型/trait 由 `org.luna.core` 声明，不是编译器内置类型身份：

| 名称 | 实际身份 | 编译器特殊协作 |
|---|---|---|
| `option::Option<T>` | 名义 enum | `for` 协议验证唯一 Core Option variant |
| Core error enums | 名义 enum | 使用通用 enum/Drop/匹配规则 |
| `iter::Iterator<Item>` | 名义 trait | `for` 静态解析唯一 Core trait |
| `IntoIterator<Item, Iter>` | 名义 trait | 隐式、唯一静态转换 |
| `FromIterator<Item, Builder>` | 名义 trait | `collect` 静态 builder 协议 |
| `Map/Filter/Take` | 名义 enum adapter | 可与编译器融合 recipe 对应 |
| `resource::Clone` | 名义 trait | 普通静态 trait/method 解析；无 `clone` intrinsic |
| `Rc<T>` / `Arc<T>` | 名义 struct | 仅使用通用 generic/Drop/trait 规则；计数策略属于 Core/Runtime |

同形状、同方法名的用户 trait 不等于 Core trait。package/module/nominal identity 是
协议选择的一部分。

## 8. 当前边界矩阵

| 类别 | 普通 host | C FFI | kernel | constexpr/反射 |
|---|---:|---:|---:|---:|
| integers/floats | 是 | 是 | 受支持标量 | 是 |
| `bool` | 是 | 否 | 结构化条件 | 是 |
| `string` | 是 | 否 | 否 | 字面量/编译期字符串 |
| `cstr` | 是 | 是 | 否 | 有限 |
| `raw<T>` | 是 | 是 | 不作为安全 device memory | 有限 |
| references | 是 | 仅受支持标量引用 | buffer borrow | 作为类型可反射 |
| product/enum/Result | 是 | 否 | 当前否 | 类型反射 |
| array/slice | 是 | 否 | 当前 kernel ABI 否 | 类型/常量信息 |
| Core `Rc`/`Arc` | 是 | 否 | 否 | 作为普通名义类型反射 |
| device buffer/event | 是 | 否 | 通过专用 ABI | 否 |
| Meta/Compiler views | 编译期 | 否 | 否 | 是 |

本矩阵描述 0.2 当前允许集合，不暗示未来永远拒绝某类边界。

## 9. 已知缺口

- 内置类型尚未由集中 registry 驱动，Parser/Sema/布局/边界集合可能发生漂移；
- `usize/isize` 的目标相关语义尚未与非 64 位平台冻结；
- 整数常量按参数宽度生成时缺少完整范围诊断；
- 高于 8 字节对齐的 inline ADT 载荷策略未冻结；
- non-Copy closure environment 和跨函数 Iterator adapter Drop 布局尚未交付；Copy-only closure environment 已按 C016 交付；
- `string` 的公开格式化、编码和标准库 API 尚未冻结；
- `device_buffer<T>` 的类型构造已泛化，但当前设备操作仍主要固定为 `i32`；
- callable ownership shape 需要更完整的赋值/统一负例矩阵。
- 参数化 Value 容器对 Meta/Compiler 类型实参的统一 well-formedness 拒绝矩阵仍需
  补齐。

这些缺口必须作为实现或规范工作处理，不能通过从清单中删除对应类型来隐藏。

## 10. 证据入口

- 类型身份：`tests/fixtures/type_relations.luna`
- 类型域：`tests/fixtures/type_domains_reflection.luna`
- 结构/名义关系：`tests/fixtures/structural_*.luna`
- 所有权：`tests/fixtures/ownership_*.luna`
- array/slice：`tests/fixtures/safe_arrays.luna`、`slice_*.luna`
- Result/enum：`tests/fixtures/result_*.luna`、`enum_match*.luna`
- Core Rc/Arc：`tests/rc_arc_core.cmake` 与 `tests/fixtures/rc_arc_core_app/`
- FFI：`tests/ffi_aot.cmake`
- kernel/event：`tests/gpu_target_split.cmake`、`tests/moon_cost_boundaries.cmake`
- Core 类型：`tests/core_surface.cmake`
- MoonIR 类型拒绝：`tests/semantic_regressions.cmake` 和 Verifier 回归
