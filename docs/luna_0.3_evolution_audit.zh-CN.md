# Luna 0.3 模型收敛与 Slot/Fragment 重构审计

[English](luna_0.3_evolution_audit.md) | 简体中文

> 文档类别：RFC / 实现审计
> 适用版本：以 Luna 0.2.1 为审计基线，讨论候选 Luna 0.3.0
> 状态：Draft
> 规范性：非规范；本文不改变 0.2.1 语言契约
> 实现核对：`8d461d4`（2026-08-01）

## 1. 范围与结论

本轮只读审计覆盖 Parser、AST、Sema、Ownership、MoonIR、Verifier、LLVM lowering、
Runtime Descriptor 和 external fragment plugin ABI。没有修改实现、测试、版本、路线图
或现有规范。

结论是**方向有条件成立，而且目标模型比当前 0.2 模型更一致**：

- 将阶段收敛为 compile-time/runtime，删除独立 `dynamic` 阶段和源码修饰语是合理的；
- 静态实体默认擦除、只为显式或可证明的运行时使用付费，与现有 static-first 原则一致；
- 将查询从 metadata 专用选择器提升为统一 Symbol Query，可以消除当前多套反射路径；
- 将 slot 固定为二等符号、仅让 runtime fragment reference 成为值，可以守住控制与数据边界；
- 默认名义类型更适合 package、插件、ABI 和 trait coherence；按需结构关系仍应保留；
- 将 `rc`/`arc` 从特殊类型种类迁移为 Core/底层容器，长期上比散落的编译器特判更健康。

但草案**不能直接进入实现**。以下问题是 Luna 0.3 开工前的硬门禁：

1. 当前 slot 是函数体内的 `SlotDeclStmt`，不是模块声明，无法拥有可导出的稳定 `SlotId`；
2. 当前 slot/fragment 返回固定为 `unit`，草案中的 `-> Response` 尚无控制语义；
3. 当前没有正式 effect contract，只有 ownership/control facts 和零散 capability flags；
4. 当前 runtime fragment 不是可保存的 typed reference，内部 fragment 也没有稳定 runtime entry；
5. 当前 plugin contract 是由名称和 `Type::toString()` 拼出的字符串，不是完整、可验证的 ContractId；
6. “Type、Resource、Query、Composition”适合作为四个语言轴，不应被实现为四种互斥根对象；
7. 默认结构类型改为默认名义类型是版本级破坏性变更，必须由 package language version/edition 隔离。

因此本审计给出的决策是：**批准继续设计，不批准立即大规模重写**。未来 1～2 周继续
0.2.1 生态链建设；0.3 只允许 RFC、迁移样例和只读/测试清点工作。

## 2. 当前实现审计

### 2.1 SlotDecl、FragmentDecl、Apply 的 AST 路径

| 层 | 当前路径 | 当前事实 |
|---|---|---|
| Token | `src/lexer/Token.h` | `Runtime`、`Dynamic`、`Slot`、`Apply`、`Select` 都是独立 token |
| Declaration parser | `src/parser/Parser.cpp:122` | 声明 retention 为 `CompileTime/Runtime/Dynamic`；`interceptor`/`context` 形成 fragment declaration |
| Statement parser | `src/parser/Parser.cpp:520` | `slot`、`apply` 是 statement；`dynamic slot/apply` 走独立分支 |
| Slot parser | `src/parser/Parser.cpp:556` | slot 必须写 interceptor/context 与 once/many；可单独声明或内联捕获 continuation |
| Apply parser | `src/parser/Parser.cpp:667` | 静态 apply 接受一个名字；dynamic apply 接受有限候选名字列表；block 可省略并作用到当前 scope 末尾 |
| AST | `src/parser/AST.h:198` | `SlotDeclStmt` 是语句，不是 `Decl` |
| AST | `src/parser/AST.h:211` | `SlotInvokeStmt` 内嵌 continuation block、动态候选和结构类型 |
| AST | `src/parser/AST.h:242` | `ApplyStmt` 保存 slot/fragment 名字、`isDynamic` 和候选列表 |
| AST | `src/parser/AST.h:525` | `FragmentDecl` 是顶层声明，保存 kind/cardinality/params/body/structuralType |

这里最关键的差异是：当前没有真正的 `SlotDecl` 顶层声明。公共库无法导出 slot 声明，
MoonIR declaration table 也没有 slot record。草案中的 `runtime slot` 不能只给
`SlotDeclStmt` 增加一个布尔字段。

### 2.2 Sema 中的 slot/fragment 类型表示

当前共同类型模型位于 `src/core/TypeSystem.h`：

- `TypeKind::Slot` 和 `TypeKind::Fragment` 都存在于通用 `Type` 中；
- 两者保存 `paramTypes`、`returnType`、parameter/result ownership contract、
  `isMultiShot` 和 `continuationKind`；
- `Type::makeSlot` / `Type::makeFragment` 将返回类型固定为 `unit`；
- control sysmeta 保存 interceptor/context、once/many、scoped-stack、forwarding 和 abort；
- 两种类型当前仍处于默认 `Value` domain，未被建模为纯 Compiler contract。

Sema 的具体 slot 环境是 `src/sema/SemanticAnalyzer.h:241` 的私有 `SlotInfo`：

```text
SlotInfo
├── local name
├── parameter types/contracts/names
├── default fragment
├── interceptor/context
├── once/many
├── implicit-capture flag
├── isDynamic
└── structural TypePtr
```

它缺少 `SlotId`、`ContractId`、返回类型、正式 effect set、visibility、package/module
ownership 和 ABI contract。`FragmentDecl::structuralType` 则描述可复用 fragment 的参数
前缀和控制类别，不保存目标 slot identity。

当前“slot 始终二等”的意图也没有完全成为 Sema 不变量：普通 identifier 分析会把任意
非函数符号的 `sym->type` 返回，slot/fragment 又被放在通用 SymbolTable 和 Value type
domain 中。后端对意外泄漏的 Slot/Fragment type 没有有效值 lowering，甚至会走通用
fallback。这是当前实现缺口，0.3 应通过实体类别和 verifier 禁令修复，而不是为 slot
补一个运行时值表示。

### 2.3 当前 SlotContract 的实际完整度

| 契约字段 | 当前状态 |
|---|---|
| identity | 只有局部字符串名；无 package/module `SlotId` |
| parameter types | 有，使用 `TypePtr`/TypeId shape |
| return type | 结构上有字段，所有实际 slot 固定为 `unit` |
| ownership | 参数 contract 已检查；result contract 实际为空/Copy Unit |
| effects | 没有正式 effect set；只有 ownership 路径检查和 capability bits |
| continuation | interceptor/context、once/many、forwarding、abort 已部分表达 |
| ABI | external plugin 使用临时字符串；没有正式 slot ABI record |
| retention | local `isDynamic` 控制 dynamic apply；不形成 runtime slot descriptor |

因此草案所称“当前每个 slot 都有完整契约”应当作为 0.3 目标，而不能描述为现状。

### 2.4 MoonIR 中的 slot、fragment、continuation

当前 MoonIR 是经过类型化的 AST-like IR：

- `moon::SlotDeclStmt` 镜像 AST 的局部声明；
- `moon::SlotInvokeStmt` 保存 slot 名、参数、结构类型和一个嵌套 `BlockStmt` continuation；
- `moon::ApplyStmt` 仍保存名字、`isDynamic` 和有限候选；
- `moon::FragmentDecl` 是带 body 的 executable declaration；
- `resume`/`abort` 是普通 statement node；
- continuation 没有独立的 typed region、frame type、入口/出口或 ABI node；
- slot 不进入 `DeclarationRecord`，fragment 会进入；
- `moon::DynamicSelectExpr` 是专门的 runtime exact-match 表示。

`src/moonir/Optimizer.cpp` 当前只重建索引，不执行 fragment composition、内联、去虚化或
continuation 优化。因此今天的静态 composition 并不发生在 MoonIR optimizer。

### 2.5 runtime retention 与 descriptor 生成位置

当前链路如下：

```text
Parser RetentionKind {CompileTime, Runtime, Dynamic}
  -> Sema metadata retention promotion
  -> MoonIR Retention {CompileTime, Runtime, Dynamic}
  -> DeclarationRecord + sysmeta.capability.runtimeRetained
  -> module feature/cost records
  -> LLVM runtime descriptor section + registry
```

具体位置：

- AST retention：`src/parser/AST.h:21`、`src/parser/AST.h:481`；
- Parser retention 和 runtime/dynamic metadata：`src/parser/Parser.cpp:122`；
- Sema attachment promotion：`src/sema/SemanticAnalyzer.cpp:558`；
- MoonIR retention 与 feature/cost：`src/moonir/Lowering.cpp:922`、`:928`；
- declaration table：`src/moonir/Lowering.cpp:980`；
- runtime section emission：`src/codegen/CodeGeneratorRuntimeDescriptors.cpp:43`。

通用 runtime descriptor 当前只编码 descriptor version、declaration/family/linkage identity、
kind、retention、retained metadata 和可选 entry。它没有序列化完整 type/sysmeta/ownership/
effect/ABI contract。

更重要的是，descriptor emitter 只从 `mFunctions` 查找 entry，而 fragment 从不被声明为
独立 LLVM function。因此 runtime-retained fragment 虽然可以获得通用 declaration
descriptor，entry 仍为空；它不是可调用的 `RuntimeFragmentDescriptor`。

### 2.6 sysmeta 与 meta 的当前职责边界

当前边界的方向基本正确：

- `src/core/SysMeta.h` 定义编译器权威、只读、强类型 facts；用户不能构造或覆盖；
- sysmeta 当前包含 control、resource、capability 和 ABI 四组 facts；
- `meta` 是用户声明的 schema，attachment 参数必须是编译期常量并接受字段类型检查；
- metadata 默认只在编译期，`runtime@...` / `dynamic@...` 才保留；
- selector 可以检查用户 metadata、声明 identity 和 callable signature。

当前缺口：

- source-level Symbol Query 不能统一查询 sysmeta；
- symbol kind、visibility、package/module、trait implementation 等事实分散在 AST、
  SymbolTable、DeclarationRecord，而不在统一 typed sysmeta schema 中；
- `Facts` 同时挂在 Type 和 declaration 上，哪些事实属于 type、symbol 或 concrete ABI
  instance 尚不够清楚；
- retention 同时有独立 enum 和 sysmeta 镜像，容易漂移；
- runtime descriptor 未携带可查询的 typed sysmeta payload。

0.3 应继续保持“sysmeta 是编译器权威、meta 是用户扩展”的边界，但不应把所有 sysmeta
塞进 `Type`。package、visibility、symbol kind 和 runtime retention 属于 Symbol；shape、
ownership signature 属于 Type/Contract；具体布局和调用约定属于 ABI projection。

### 2.7 静态 apply 的 lowering 与优化路径

当前真实路径为：

```text
Parser ApplyStmt
  -> Sema lexical mApplyScopes / contract kind+cardinality check
  -> fragment 按具体 slot 再分析参数、resume/abort 路径
  -> OwnershipChecker 模拟 fragment 与 continuation 的所有出口
  -> MoonIR 原样保留 ApplyStmt + SlotInvokeStmt + nested continuation
  -> MoonIR optimizer 只 canonicalize
  -> LLVM codegen 再次维护 apply scopes
  -> generateFragmentInline 直接生成 fragment body 与 continuation CFG
  -> LLVM O2/O3 消除临时 frame/branch
```

优点是静态路径确实没有通用 runtime dispatch，现有 ABI 回归也检查了这一点。缺点是
composition 过晚：MoonIR Verifier 看不到完成组合后的 handler/continuation CFG，Moon
Container 也无法复用当前 codegen 内部逻辑。0.3 应把静态 composition 变成验证过的
MoonIR-to-MoonIR pass，LLVM backend 只消费已决定的 control operations。

### 2.8 runtime fragment 当前可用能力

当前有三种容易混淆的能力：

1. `runtime context/interceptor`：只让声明进入通用 runtime descriptor；不产生 callable
   fragment entry 或 fragment reference；
2. `dynamic apply` 的 linked candidates：候选集合仍在编译期有限且全部被内联到分支；
   runtime 只按环境变量选择名字；
3. external plugin v1：动态库提供 C descriptor 和 entry，host 验证后调用。

external plugin v1 的边界是：

- 仅 host-only、single-shot interceptor；
- 接收只读 explicit argument pointers；
- 返回 continue/abort/error；
- 不接收 Luna continuation，不支持 `resume()`、context、many、capture；
- 动态库保持到进程结束，没有 unload/reclamation；
- contract 字符串由 `slot name + kind + cardinality + Type::toString()` 构成；
- contract 没有覆盖 package `SlotId`、ownership relation/usage、return、effect set、
  target layout ABI 或 continuation ABI。

所以草案中的 runtime fragment query、保存/传递、typed reference、stable entry、lexical
runtime apply 和 module lifetime 都是新能力。plugin v1 可以成为兼容 adapter，但不能
被当作该模型已经存在的证据。

## 3. 四域收敛的设计评估

### 3.1 四个“语言轴”，不是四种万能实体

建议使用以下内部关系：

```text
SymbolRecord
├── SymbolId / SymbolKind / package / module / visibility
├── TypeRef 或 ContractRef
├── typed sysmeta
├── user meta attachments
└── retention / runtime descriptor policy

TypeRecord
├── TypeId
├── ShapeId
├── nominal identity
├── constraints / traits
└── ABI-independent semantic shape

ResourceContract
├── relation: owned/shared_borrow/mutable_borrow
├── usage: Copy/Affine/Linear
├── cleanup/drop capability
└── lifetime / allocator domain

ControlContract
└── SlotContract + Fragment capability + continuation rules
```

Type、Resource、Symbol Query、Composition 可以作为用户理解语言的四个中心，但 Symbol
仍是连接它们的载体。尤其 meta/sysmeta 既能描述 type，也能描述 function、slot、fragment、
impl 和 module；不能全部归属 Type。

### 3.2 Type：默认名义、按需结构

该方向比当前默认结构更适合长期生态：同形状的两个公开数据模型不会意外共享 TypeId、
generic instance、trait coherence 或 plugin contract。

建议 0.3 规则：

- 具名 `struct`/`enum` 默认名义；
- function、tuple/anonymous record、slot contract shape 等自然结构类型继续结构化；
- “按需结构”优先使用匿名 record/shape constraint/`same_shape` 关系，不急于增加
  `structural` 关键字；
- 若以后需要具名结构 alias，再通过独立 RFC 决定语法；
- 0.2 源码只有在 package manifest 明确选择 `language = "0.3"` 后才采用新默认；
- 0.2 package 不能因新编译器版本而静默改变 TypeId。

### 3.3 Resource：保留正交关系，容器退出 TypeKind 特判

建议保留现有正确部分：relation 与 usage 正交，`affine`/`linear` 不改变 TypeId。

`Box<T>`、`Rc<T>`、`Arc<T>` 应成为 Core/底层名义容器类型，通过编译器认可的最小
resource protocol 提供 layout、clone/retain、Drop 和 allocator-domain facts。迁移前必须
先完成：

- 通用 Drop glue 和递归容器布局；
- Core generic nominal container 的可靠 codegen；
- `Clone`/retain 的静态 trait 或 intrinsic boundary；
- allocator domain 与 thread-safety sysmeta；
- 0.2 `rc new` / `arc new` 的兼容 lowering。

不要把 `Rc`/`Arc` 从 TypeKind 删除与 slot/runtime ABI 重构放在同一提交中。

### 3.4 Symbol Query：从“选一个函数”变成 typed symbol set

当前 selector 只处理同一 function family，静态结果必须唯一；runtime 路径又是专用
exact-match。目标查询模型应先产生集合，再由显式 cardinality terminal 决定结果：

```text
compile_symbols -> SymbolSet<CompileTime, K, C>
runtime_symbols -> SymbolSet<Runtime, K, C>

terminal: all / one / optional / first-with-explicit-order
```

建议复用现有 `select` 关键字，不增加 `query` 关键字。具体语法留给 RFC，但语义应为：

- view 决定 compile-time 或 runtime，不用 `dynamic select`；
- kind/type/trait/contract/sysmeta 条件由编译器检查；
- `meta<M>` 条件由用户 schema 检查；
- compile-time 返回 `SymbolRef`/`SymbolSet`，必须在 MoonIR 前擦除或静态解析；
- runtime 只返回 descriptor-backed typed `RuntimeRef`；
- 查询本身不承诺唯一，`.one()` 才承担 no-match/ambiguous 语义；
- 查询排序必须显式，不能依赖链接或注册顺序。

### 3.5 Composition：slot/fragment 是底座，不承担服务发现策略

slot/fragment 适合表达受限控制位置和 handler，不应承担普通依赖注入、对象构造、服务
生命周期或全局替换。

不建议增加 `service`/`provider` 关键字。可以复用：

- `trait`：服务的类型化操作契约；
- `impl Trait for Type`：provider 的实现事实；
- user `@provider(...)` metadata：名称、优先级、route 等策略；
- typed Symbol Query：按 `implements<Trait>` sysmeta 和 `meta<provider>` 发现；
- fragment：仅当 provider 需要改变 continuation/control flow 时使用。

`@provider` 不能赋予类型安全或 capability；安全来自 trait/slot contract 的 sysmeta
匹配。runtime provider 若要成为普通可调用引用，需要独立的 runtime trait/service ABI，
不应偷渡进 RuntimeFragmentRef。

## 4. 候选 Luna 0.3 Slot/Fragment 模型

### 4.1 必须先冻结的语义

1. slot 是 package/module 级可命名声明还是只能局部声明；建议模块契约 + 局部 invoke；
2. `slot (...) -> R` 的 R 是 handler 返回、continuation 返回、还是整个 apply expression 返回；
3. interceptor/context 是否继续作为源码关键字，或成为 `fragment` 的 control sysmeta；
4. fragment 是否名义绑定一个 SlotId，还是只满足可复用结构 ContractId；
5. effect set 的最小闭集和兼容关系；
6. once/many、abort、return、`?` 和 cleanup 的交互；
7. runtime fragment reference 的 Copy/Affine 规则和 module lease；
8. runtime context 的 continuation ABI；首个 0.3 runtime ABI 建议只支持 single-shot interceptor。

在这些问题冻结前，不应调整语法。

### 4.2 推荐的实体边界

- `SlotDecl`：模块级二等 Symbol，拥有 nominal `SlotId` 和完整 `SlotContract`；
- `SlotInvoke`：函数体 operation，引用固定 SlotId 并携带 lexical continuation region；
- `FragmentDecl`：静态 handler entity，默认没有 runtime identity；
- `RuntimeFragmentDescriptor`：仅为 retained fragment 生成的 ABI record；
- `RuntimeFragmentRef<S>`：普通 runtime value，指向 compatible descriptor 并持有 module lease；
- `apply`：编译器内建 operation；operand 是静态 Fragment symbol ref 或 runtime fragment ref；
- slot symbol 永远不产生普通 runtime value，也不存在 `RuntimeSlotRef` 传递模型。

`RuntimeFragmentRef<request>` 中的 `request` 是 type-position 的 symbol/contract projection，
不是 slot value。

### 4.3 RuntimeSlotDescriptor 最小数据模型

建议最小 v1 模型为：

```text
DescriptorHeader
├── magic
├── abi_major / abi_minor
└── struct_size

RuntimeSlotDescriptor
├── header
├── SlotId                       # nominal target identity
├── ContractId                   # canonical semantic contract digest
├── owner ModuleId
├── flags                        # visibility/host/runtime capabilities
├── parameter_count
├── RuntimeParameterContract[]
│   ├── TypeId
│   ├── AbiLayoutId
│   ├── ownership relation
│   └── usage
├── RuntimeResultContract
├── EffectSetId / typed effect bits
├── control form + cardinality + abort/forwarding rules
├── CallAbiId
└── ContinuationAbiId            # none for ABI that cannot expose continuation
```

同时需要最小 fragment/ref 配对模型：

```text
RuntimeFragmentDescriptor
├── header
├── FragmentId
├── target SlotId                # 防止同形 slot 被意外互换
├── ContractId
├── stable entry
├── capability/effect flags
├── CallAbiId / ContinuationAbiId
└── owner ModuleId + lifetime policy

RuntimeFragmentRef<S>
├── descriptor pointer/registry handle
└── ModuleLease                  # ref 存活时 entry 不得回收
```

`ContractId` 必须覆盖 parameter/result TypeId、ownership、effects、control/cardinality 和
ABI projections；不能只覆盖类型拼写。`SlotId` 与 `ContractId` 必须同时验证：前者回答
“是不是同一个具名控制位置”，后者回答“契约是否兼容”。ID 不能只信任短 hash；MoonIR
和 loader 应能比较或重新计算 canonical payload。

### 4.4 自动推导与显式 runtime slot 的边界

| 场景 | 规则 |
|---|---|
| private、闭世界 slot，只绑定静态 fragment | 不生成 RuntimeSlotDescriptor |
| private slot 的 apply operand 可能来自 `RuntimeFragmentRef<S>` | 自动保留 slot descriptor |
| private slot 被同模块 runtime fragment/exported plugin descriptor 目标引用 | 可由可达性自动保留，并记录 cost reason |
| exported/public slot | 只有显式 `runtime slot`/等价 sysmeta export 才形成稳定 ABI |
| exported runtime fragment 指向 public static-only slot | 编译错误；不能静默改变依赖 ABI |
| generic/public API 接受 RuntimeFragmentRef | API 必须显式声明 runtime slot contract |
| dependency 中的 runtime slot | 只信任依赖 manifest/descriptor，不做跨包闭世界猜测 |

闭世界推导只能减少 private implementation 的产物，不能决定公共 ABI 是否存在。

### 4.5 统一 apply 的 lowering

建议 MoonIR 显式区分 operand 的已验证形态，而不是保留源码 `dynamic` 标志：

```text
apply.static  SlotId, FragmentId, continuation.region
apply.runtime SlotId, RuntimeFragmentRef, continuation.region
```

这是 IR operation 的 lowering 分类，不是两套源码语义。两者共享同一个 `SlotContract`
compatibility checker、ownership checker 和 cleanup verifier。

静态路径：

```text
contract check
-> continuation region formation
-> handler composition
-> ownership/effect verification
-> MoonIR optimization
-> direct CFG/codegen
```

运行时路径：

```text
typed ref check
-> descriptor SlotId + ContractId + ABI validation
-> scoped handler frame
-> continuation execution
-> deterministic cleanup + module lease release
```

源码 `apply` 应优先要求显式 block。0.2 的“无 block，作用到当前 scope 末尾”可以在
迁移期 desugar，但不应继续扩展为 global remove/replace。

## 5. Luna 0.3 迭代目标与非目标

### 5.1 0.3.0 必须交付

1. 两阶段模型：compile-time/runtime；源码与核心 IR 不再有 Dynamic retention；
2. 统一 Symbol Catalog 与 typed query，至少完整支持 compile-time symbols；
3. runtime_symbols 只暴露 descriptor-backed typed references；
4. sysmeta/meta 查询职责正式分离，安全判定不依赖用户字符串；
5. package language version/edition 隔离默认名义类型变化；
6. ResourceContract 成为统一 ownership/lifetime 接口，Box/Rc/Arc 容器迁移有兼容桥；
7. 模块级 SlotDecl、完整 SlotContract、稳定 SlotId/ContractId；
8. 静态 apply 在 MoonIR 中完成组合并保持零 runtime descriptor/dispatch 成本；
9. runtime descriptor v2、typed RuntimeFragmentRef 和 lexical runtime apply；
10. 首版 runtime apply 至少稳定支持 host-only、single-shot interceptor；
11. 0.2 dynamic syntax 有明确诊断和迁移工具，不静默改变含义；
12. JIT/AOT、plugin、ownership、cost boundary 和 ABI 都有正负回归。

### 5.2 不作为 0.3.0 发布门

- 完整 Moon Container、hotspot JIT、PGO、去优化和热替换；
- external runtime context、multi-shot continuation ABI；
- 全局 remove/replace/weaving；
- 任意 native function pointer 作为 fragment；
- plugin unload/reload 的完整策略；但 reference 必须预留安全 module lease；
- `service`/`provider` 新关键字；
- 通用 runtime trait object/service ABI；
- 所有 symbol kind 的开放式 runtime reflection；
- 把 slot 变成普通值。

Moon Container 应在普通 Runtime 能正确执行 runtime apply 之后单独迭代。0.3 只需让
descriptor、MoonIR control ops 和 module lifetime 足以成为其稳定输入。

## 6. 现有边界与迭代后边界

| 维度 | 0.2.1 当前 | 候选 0.3.0 |
|---|---|---|
| 阶段 | CompileTime/Runtime/Dynamic retention | Compile-time/Runtime；运行时行为由值、descriptor、builtins 决定 |
| 类型默认 | named struct/enum 默认结构 | named struct/enum 默认名义；结构关系按需 |
| Rc/Arc | TypeKind、parser、Sema、codegen 特判 | Core/底层名义容器 + resource protocol |
| Symbol Query | function family + metadata selector | 所有 symbol kind 的 typed set/query |
| 查询 cardinality | selector 必须返回一个 | set 默认；one/optional/all 显式 |
| sysmeta | typed 但分散，主要内部使用 | typed Symbol/Type/Contract/ABI projections，可统一查询 |
| meta | 用户 typed schema，主要服务 selector | 保持用户扩展，不承担安全契约 |
| slot | local statement、局部名、unit result | module symbol、SlotId、完整 contract，仍二等 |
| fragment | top-level entity，静态展开；retention 不等于 callable | 静态 handler；runtime ref/descriptor 是独立具象化 |
| static apply | Sema/ownership 检查，LLVM codegen 才组合 | verified MoonIR composition pass |
| runtime apply | finite linked candidates + name branch | typed RuntimeFragmentRef + scoped handler frame |
| plugin | v1 C interceptor，process lifetime | v1 adapter；v2 descriptor/ref/lease，context 后续 |
| runtime descriptor | generic declaration record，contract 不完整 | common header + kind-specific typed descriptor |
| Moon Container | 尚未实现 | 非 0.3 发布门；消费已验证 MoonIR/descriptor |

## 7. 分阶段、可回滚的提交计划

每个阶段必须是独立提交组，保留上一条路径直到 parity tests 通过；不得在一个提交中
同时改变源码语义、MoonIR format 和 Runtime ABI。

### R0：设计冻结（当前 0.2 周期，只写文档）

- 决定第 4.1 节八个语义问题；
- 建立 0.2 migration corpus 和预期 0.3 结果；
- 为 language version/edition 写独立 RFC；
- 不改变 parser 或运行时行为。

回滚：删除 Draft 文档即可，无产物影响。

### R1：只加 shadow identity/contract 模型

- 增加 `SymbolId`、`SlotId`、`ContractId`、typed `SlotContract`；
- 从现有 SlotInfo/Type 派生并比对，不改变 codegen；
- Verifier 检查 canonical payload 与 digest；
- 保留旧 `isDynamic` 和旧 contract string。

回滚：删除 shadow records，旧路径完全不变。

### R2：修复二等实体边界

- 将 Slot/Fragment contract 移出普通 Value domain，或增加明确 non-value entity class；
- Sema 拒绝赋值、返回、普通参数、容器存储；
- 引入单独 `RuntimeFragmentRef` value kind，但暂不执行 runtime apply；
- 补齐 frontend/MoonIR negative matrix。

回滚：兼容 flag 恢复旧 Sema；不触碰 ABI。

### R3：MoonIR 静态 composition

- 引入 SlotDecl record、continuation region、static apply/control ops；
- 新 MoonIR pass 完成 handler composition；
- codegen 新旧路径并行，做 JIT/AOT/LLVM parity；
- 新路径稳定后删除 codegen 的 AST-like scope lookup。

回滚：切回旧 lowering；源码与 Runtime ABI 未变。

### R4：compile_symbols 统一查询

- 建立统一 Symbol Catalog，接入 function/type/trait/impl/slot/fragment/meta；
- 先实现 compile-time typed set、sysmeta/meta predicates 和 explicit cardinality；
- 旧 `select family with selector` desugar 到新 catalog；
- 静态结果保持完全擦除。

回滚：旧 selector engine 仍可独立工作。

### R5：language version 与默认名义迁移

- manifest 增加显式 language version/edition；
- 0.2 模式保持结构默认，0.3 模式采用名义默认；
- 提供 TypeId/ShapeId 和 generic-instantiation 差异诊断；
- 不与 Rc/Arc 或 slot ABI 改动混合。

回滚：选择 0.2 language mode；包身份不变。

### R6：Resource 容器桥接

- 先引入 Core `Box/Rc/Arc` 名义容器和 resource protocol；
- 旧 `rc new`/`arc new` desugar 到 Core 构造；
- parity 后逐步删除 TypeKind/Sema/codegen 特判；
- 保持 Runtime allocator/retain/release ABI adapter。

回滚：desugar 切回旧 intrinsic 类型。

### R7：Runtime Descriptor v2 与 runtime_symbols

- 先生成 v2 slot/fragment/type/contract descriptors，不执行 apply；
- registry loader 验证 header、canonical identities、ABI、module ownership；
- runtime_symbols 返回 typed refs，生命周期由 ModuleLease 保护；
- v1 descriptor/plugin 保持并行兼容。

回滚：runtime query 禁用，v1 仍可用。

### R8：统一 runtime apply

- 首先只支持 host-only、single-shot interceptor；
- apply operand type 决定 static/runtime lowering，无 `dynamic apply`；
- descriptor validation、handler frame、cleanup、错误快照和 lease release 全覆盖；
- external plugin v1 通过 adapter 进入受限 v2 fragment capability。

回滚：禁用 RuntimeFragmentRef apply，保留 static path 和 v1 adapter。

### R9：删除 compatibility surface

- 只有 migration telemetry、文档和测试显示不再需要后，才删除 Dynamic retention、
  `dynamic select/apply/slot` parser 分支和旧 MoonIR fields；
- bump MoonIR format/Runtime ABI 时保留明确版本拒绝；
- 更新语义基线、参考、CHANGELOG 和双语文档。

回滚：此阶段不可用简单代码回滚恢复已发布 ABI，必须在发布前完成兼容窗口验证。

### R10：Moon Container（独立项目）

- MoonIR serialization/loading/reverification；
- runtime apply devirtualization、specialization、profile recompilation；
- deoptimization、module versioning 和 code reclamation。

它不应阻塞普通 Runtime 的正确 runtime apply，也不应与 R8 合并提交。

## 8. 验收门与暂停条件

任何一项不满足都应暂停 0.3 实现：

- slot result/continuation/return/abort 语义没有规范和路径矩阵；
- SlotId、ContractId、TypeId、ShapeId、AbiLayoutId 仍被混用；
- runtime fragment ref 没有 module lifetime 证明；
- runtime descriptor 仍依赖 `Type::toString()` 或用户 metadata 做安全判断；
- public slot descriptor 由闭世界使用分析隐式决定；
- runtime apply 的所有权结果依赖“当前候选集合恰好相同”，而不是 contract effect；
- static apply 需要 runtime descriptor 才能通过 verifier；
- 默认名义迁移会改变未选择 0.3 language mode 的 0.2 package；
- Rc/Arc 容器迁移尚无 Drop/allocator/thread-safety 证明；
- 为了表达 service/provider 而让 slot 成为值或增加全局 replace 状态。

## 9. 近期建议（继续 0.2.1）

未来 1～2 周继续生态链计划，不做 0.3 实现。允许的准备工作只有：

1. 评审并冻结第 4.1 节的语义问题；
2. 收集现有 slot/fragment/select/ownership/plugin fixtures 作为迁移 corpus；
3. 为 formatter/LSP/文档工具整理只读 symbol inventory 接口需求，但不改变语言；
4. 继续 0.2 diagnostics、package、build/test、发布和编辑器生态工作；
5. 0.3 代码放在明确的后续分支/里程碑，不混入 0.2 修复提交。

最终建议：保留草案的核心原则，但把 0.3 定义为“语义核与边界收敛版本”，不是一次性
交付完整 Moon Container、通用 runtime reflection 和所有 continuation ABI 的动态系统版本。
