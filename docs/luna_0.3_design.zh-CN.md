# Luna 0.3 总体设计草案

[English](luna_0.3_design.md) | 简体中文

> 文档类别：RFC / 总体设计
> 适用版本：候选 Luna 0.3.0
> 状态：Draft
> 规范性：非规范 RFC；实现完成记录会标出已在 0.3 开发期编译器生效的部分
> 最终 0.2 实现检查点：`a188d87a6f10d7fa67582389a0a0b915f3741401`（2026-08-09）

本文是 Luna 0.3 的总纲。现有
[Slot/Fragment 重构审计](luna_0.3_evolution_audit.zh-CN.md)是受本文约束的专题审计，
不是 0.3 的总规范。

文中使用以下状态：

- **Confirmed**：项目负责人已经明确确认的方向；
- **Proposed**：有具体建议，但尚未冻结；
- **TBD-xxx**：必须在实现对应能力前决定的稳定占位。

所有计划语法都只是 Draft。实现、测试、参考文档和变更日志同步完成前，本文不声明
编译器已经支持这些能力。

### 已冻结的决定边界（2026-08-09）

本文的 Confirmed ID 是实现授权。下列四个 ID 是完整的未决集合；实现不得隐式替它们
选择答案。后续发现的新歧义必须先在此获得稳定 `TBD-*` ID，之后才能编写依赖代码。

| ID | 尚需决定 | 阻塞 | 不阻塞 |
|---|---|---|---|
| `TBD-M005` | binary magic、整数编码、section、alignment、压缩和签名细节 | Moon 序列化格式 conformance、parser、signer 和最终 `-t moon` 输出 | 内存 canonical MoonIR table、结构 verifier 规则或确定性 model 测试 |
| `TBD-EV004` | pinned/switchable/initializer/activation 的源码与 API 拼写 | 公开 evolution API 及最终源码/runtime binding | generation identity、module lease、staging invariant 或内部状态机测试 |
| `TBD-Q004` | `.all()` 顺序和显式排序 API | 公开 `.all()` 语义及其 ABI | Symbol Catalog、typed query set、`.one()` 或 `.optional()` |
| `TBD-SF006` | module Slot/Fragment 语法和 single-shot control 的精确交互 | 新 Slot/Fragment parsing、control semantics 与公开 runtime-apply surface | shadow SlotId/ContractId、descriptor schema 或旧版 corpus 固化 |

当前没有登记 Proposed 决定。新的 Proposed 项必须明确标记并加入本边界，才能影响实现。

本草案使用以下规范术语：

| 术语 | 含义及排除项 |
|---|---|
| compile-time / runtime | 仅有的语言阶段；“动态”只作为解释性文字 |
| sysmeta | 编译器推导、只读、强类型的事实；不是 effect 机制或 user metadata |
| user `meta` | 策略/应用 metadata；不得作为安全证据 |
| artifact target | `-t` 选择的产物级别；不是 language mode 或语义版本选择器 |
| Moon Container / Luna Native / Foreign C FFI | 本地验证后安全 / 由证明建立信任 / unsafe foreign implementation 三种边界 |
| relation / usage | ownership relation 与 Copy/Affine/Linear 消耗纪律；两者正交 |
| slot / RuntimeFragmentRef | 二等 control symbol / typed runtime value；都不等同普通函数 |
| MoonIR | 同时具有内存和序列化形式的单一 canonical backend IR，不是两个语义 IR |
| unchecked operation | 未来可能出现的窄粒度 primitive-specific 省略检查；绝不是通用 `unsafe {}` scope |

## 1. 定位与基本原则

### C001：语言定位（Confirmed）

Luna 是静态优先、安全验证、高性能的系统编程语言。普通程序只为实际使用的能力
付费；语言通过显式运行时能力支持按需接入新实现和新代码，并通过 Moon Container
及 MoonRuntime 支持受控进化。异构与并行计算属于原生可选执行能力，但不是 0.3.0
的优先扩展目标。

### C002：明确断代（Confirmed）

Luna 0.3 是相对预发布 0.2 编译器的明确破坏性更新：

- 不增加 `language = "0.2"`、edition 或兼容模式；
- 0.3 编译器不保留旧语法、旧 retention、旧类型默认或旧 lowering 分支；
- 需要编译 0.2 源码的用户应使用 0.2 编译器；
- 迁移通过变更说明、迁移表和必要时的独立迁移工具完成，不通过主编译器双轨实现；
- 0.3 发布必须显式列出每项破坏性变化。

该决定的理由是项目尚未正式发布，也没有需要主编译器长期承载的既有生态。此时保留
双轨语义只会扩大 Parser、Sema、MoonIR、Runtime、测试和文档的永久维护面，违背
编译器轻量、纯粹的目标。

### C003：静态优先与按需付费（Confirmed）

- 编译期可决定的行为不得推迟到运行时；
- 编译期实体默认擦除；
- 未使用 Runtime、registry、Moon loader、GPU 或 executable-memory 能力的程序不得
  携带对应产物和初始化成本；
- 安全验证发生在编译、安装、装载或绑定边界，不应进入普通函数调用热路径；
- 成本必须能由产物、symbol、descriptor 和 benchmark 证据解释。

## 2. 阶段、sysmeta 与用户 metadata

### C004：只有 compile-time/runtime 两个阶段（Confirmed）

0.3 删除独立 `dynamic` retention 和对应源码修饰语。运行时发现、引用、装载或切换
由具体的 runtime value、descriptor 和 builtin operation 表达，不形成第三个语言阶段。
“动态”可以作为解释性中文术语，但不成为类型域或 retention。

### C005：不引入 effect 机制（Confirmed）

Luna 不引入显式 effect annotation、effect row、effect set 或用户可声明的 effect
contract。Slot/Fragment 的目的仍是控制流与扩展性，不是向语言加入代数效应系统；
其实现可以借鉴代数效应的控制思想，但不公开 effect 语言机制。

编译器从程序结构推导安全和 lowering 所需事实，并将其中允许公开的部分记录为只读、
强类型 sysmeta：

- 用户最多读取或查询 sysmeta，不能构造、覆盖或伪造；
- control、resource、host/device、FFI、runtime retention、suspension 和 ABI 等事实属于
  编译器推导的 sysmeta，而不是 effect；
- loader 和 verifier 检查 descriptor/MoonIR 中的 sysmeta 与代码事实一致；
- user `meta` 继续承载版本、标签、路由等策略信息，不参与安全判定；
- host capability 是 Runtime 授权策略，不是源码 effect 系统。

`SM001`（Confirmed）：sysmeta 使用封闭、编译器拥有的强类型 schema，至少划分 identity、
resource、control、ABI/target 和 retention 命名空间。字段分别标记为 compiler-internal、
container-stable 或 user-queryable；只有验证、装载和绑定所需的事实进入产物。用户不能
扩展该 schema，也不能把 user meta 投影成安全事实。

## 3. 产物与信任边界

### C006：单一 `-t` 产物目标选项（Confirmed）

`luna build` 使用 `-t`（target）选择目标产物级别。`-t` 只选择产物，不改变源语言
语义，也不得成为绕过类型或所有权检查的开关。

`T001`（Confirmed）：`-t native` 是默认值，另外提供 `-t moon` 和 `-t cffi`。
`cffi` 产物不携带 Luna Native 信任证明，因此接入 Luna 时与其他 Foreign C FFI 一样
处理。

`T002`（Confirmed）：package manifest 决定 application/library 种类，`-t` 只决定产物
级别，不再编码 static/shared 等链接形态。`main` 和 export surface 参与合法性检查，
不静默改变 `-t`。

`T003`（Confirmed）：产生正式产物的 `luna build` 必须以 `luna.package` 为输入；standalone
file 仍可用于 `check`、`run` 和 `analyze`。manifest 必须且只能选择
`kind = "application"` 或 `kind = "library"`；不含 language/edition 字段，0.3.0 也不增加
linkage 配置。只有 `build` 接受 `-t`，`-o` 可以覆盖完整默认输出路径。用于诊断的
`--emit-moonir` 与 sealed `-t moon` 产物保持分离。

0.3.0 的 target/package 矩阵固定如下：

| Package kind | `-t native` | `-t moon` | `-t cffi` |
|---|---|---|---|
| `application` | 平台 executable；package 恰有一个 `main` | 带恰好一个 `main` entry 的 `.moon` | 非法 |
| `library` | trusted、可装载 shared library；无 `main` | 无 `main` 的 `.moon` library | C ABI shared library + 生成的 C header；无 `main` |

Native/CFFI library 使用平台普通 shared-library 约定：ELF 平台为 `lib<name>.so`，macOS
为 `lib<name>.dylib`，Windows 为 `<name>.dll` 以及所需 import `.lib`。Native application
使用平台 executable 约定。两类 Moon package 都使用 `<name>.moon`，由 manifest entry
区分 entrypoint 规则。`<name>` 默认取 Package ID 最后一段；未给 `-o` 时输出位于
`<package-root>/build/<target>/`。

0.3.0 有意不提供可分发的 machine-code static-library 产物。静态优先的 application
从源码或 canonical MoonIR 组合 dependency 后生成最终 application，避免在 MVP 同时
引入 archive proof 与重复链接策略。Luna Native shared library 将证明嵌入平台 binary
section；计算自身 digest 时按规范排除 proof section，而证明绑定其余所有 loadable
code/data 与 typed descriptor。section 缺失或损坏时 Native loader 必须拒绝，不搜索
proof sidecar。

`-t cffi` 只导出显式声明为 `export "C" fn` 的函数；普通 `export fn` 保持 Luna typed
ABI，因此出现在 CFFI library public surface 时非法。每个 C export 必须属于封闭的
C-ABI-safe 类型子集，生成的 `<name>.h` 声明编译器选定的真实 link symbol，避免
metadata/module identity 与 binary 不一致。至少需要一个 C export。保留显式 ABI
拼写是为了让 source ABI 与 `-t` 正交，不是兼容模式。

Moon package 在一个扁平 manifest section 中声明获准的 host import：

```toml
[host-imports]
"io::write" = "org.luna.host.console.write"
```

key 是 package-local、module-qualified 的 `extern "C"` declaration name，value 是稳定
host capability ID。编译器从 declaration 推导 typed ContractId，用户不书写或覆盖它。
`-t moon` 必须拒绝未声明的 foreign dependency、无法表示为 typed host import 的已列
declaration、contract 不同却 link symbol 相同的重复项，以及该 section 中的任何
path/library name。MoonRuntime host policy 根据 import identity、ContractId 与 capability
绑定实现。

### C007：三种信任边界（Confirmed）

| 边界 | 定位 | 安全来源 |
|---|---|---|
| Moon Container | Safe / verified | 本地 Moon verifier 对容器重新验证 |
| Luna Native | Trusted | 来自可信 Luna 编译链，并由绑定机器码的证明确认来源 |
| Foreign C FFI | Unsafe classification（不是语法） | Luna 只检查声明的 ABI，调用方承担外部实现风险 |

- 不设计隔离 Native、Native sandbox 或 IPC 执行层；
- Luna Native 不能只靠自报 header 获得可信身份；证明必须绑定实际代码和数据；
- 缺少或损坏 Luna 证明的原生库不能由 module loader 自动降级；安全 loader 必须拒绝；
- 同一文件仍可由用户通过显式 C FFI 路径作为 unsafe foreign library 使用；
- 其他语言产生的 C FFI 库属于 unsafe；Luna 产生且证明有效的 Native 属于 trusted；
  基于 Moon Container 并通过本地验证的代码属于 safe。

`TR001`（Confirmed）：Luna Native 证明覆盖 loadable code/data digest、导出 typed
descriptor、ContractId、target ABI、compiler identity 和动态 foreign dependency
清单。证明负责把描述绑定到实际产物；信任来自安装记录或显式 trust store，而不是
artifact 自报。缓存以内容摘要为基础，dependency 或 trust 改变后不得复用旧结果。

`TR002`（Confirmed）：Luna 不提供能关闭内部类型、所有权或 Moon 验证的通用
`unsafe {}`。`extern "C"` 声明本身就是 Foreign C FFI 边界；编译器仍检查其声明 ABI
和每个调用的静态类型，但不声称验证外部实现。若未来个别 primitive 需要省略特定
动态检查，应使用窄粒度 `unchecked_*` operation，而不是可传播的 unsafe scope。
Foreign C FFI 的结果不得被包装成安全 `ModuleRef`。`-t moon` 不允许任意解析
`extern "C"` symbol；它必须拒绝该依赖，或把它 lowering 为 manifest-declared、由 host
policy 显式授权的 typed import。优化器默认把 foreign call 视为有未知副作用；除 ABI
和显式 foreign contract 外，不从 user annotation 推导 pure、noalias、nothrow 或
lifetime 保证。

## 4. 类型与 Resource

### C008：具名类型默认名义（Confirmed）

- 具名 `struct`/`enum` 默认形成名义 TypeId；
- trait、metadata schema 和具名运行时 contract 始终具有声明身份；
- anonymous record、tuple、function shape 和显式 shape relation 可以继续结构化；
- TypeId、ShapeId、AbiLayoutId 和 ContractId 必须分开；
- 0.3 不保留 0.2 默认结构语义的编译模式；
- `nominal` 在 0.3 中不是关键字或声明修饰符：`struct` 与 `enum` 已经表达全部所需的
  identity 语义。

`TY001`（Confirmed）：匿名 record/tuple/function shape 保持结构化；具名类型即使 layout
相同也不发生隐式结构转换。shape constraint 可以检查结构关系，但不会抹除 TypeId；
具名类型之间必须显式构造或投影。

`TY002`（Confirmed）：匿名 record 不使用 `record` 关键字。类型和值分别写作
`{ x: i32, y: i32 }` 与 `{ x: 1, y: 2 }`。`Point { x: value.x,
y: value.y }` 显式构造具名值，`{ x: point.x, y: point.y }` 显式投影为匿名 record。
具名值与匿名 record、两个不同的具名值之间，都不会仅因字段相同而隐式转换。

record 字段名必须唯一。initializer 按源码顺序执行，但字段 identity、ShapeId、TypeId
与物理布局统一使用按名称规范化的顺序，因此字段书写顺序不能改变结构类型或 ABI。
grammar context 区分 record 与 block：要求 block 的位置把 `{ ... }` 解释为 block，要求
expression/type 的位置解释为 record。statement 开头的 `{` 仍优先开始 block；需要独立
record expression 时可以加括号。0.3 不增加不受限制的裸 block expression，以免重新产生
歧义。

具名 `constraint` declaration 仍是唯一公开的编译期 proposition 机制。C++ concept 风格的
constrained parameter、具名 `where Constraint<T>` clause 与 inline `where` predicate 都在
frontend analysis 中归一化为同一 constraint predicate。inline form 是类似 lambda 的匿名
拼写：没有公开 SymbolId，不进入 Symbol Catalog，并在 MoonIR 前擦除。结构条件使用既有
type-relation predicate，例如 `type_same_shape::<T, { x: f64, y: f64 }>()`；`where` 不引入
独立 ShapeConstraint、effect、runtime contract 或 TypeKind。trait behavior bound 即使也由
`where` 承载表面拼写，仍保留独立的 trait 语义。

### C009：relation 与 usage 正交（Confirmed）

Ownership relation 继续表示 owned/shared borrow/mutable borrow，usage 继续表示
Copy/Affine/Linear。`affine`/`linear` 修饰 binding contract，不改变 TypeId。

### C010：`linear {}` / `affine {}`（Confirmed）

块状语法是纯语法糖：块内新声明变量默认使用对应 usage。Sema 固化每个 binding 的
最终 usage 后，MoonIR 不保留 usage block 节点，因此没有运行时成本。

示例：

```luna
linear {
    let transaction = begin_transaction();
    let token = acquire_token();

    affine let cache = rc(data);
}
```

`US001`（Confirmed）：显式覆盖语法为 `copy let`、`affine let` 和 `linear let`。显式
binding contract 替换 block default，可以选择类型/Resource contract 允许的任一
usage，但不能弱化其固有要求。
显式写出的较弱 contract 会直接拒绝，而不是静默提升。`copy {}` 不是 usage
block 形式，也不保留 qualifier 写在 `let` 之后的语法。

`US002`（Confirmed）：普通嵌套块继承当前 usage default，嵌套 `linear {}`/
`affine {}` 覆盖它。默认适用于块内新建的 local、pattern 和 loop binding；borrow
relation 与 usage 正交，因此 borrow binding 也取得该默认值，但仍受 borrow checker
约束。lambda/局部函数参数从 Copy default 重新开始，capture 保留被捕获 binding 的
既有 contract。

实现规则：parser 只把词法范围的 default 传到受影响 binder；Sema 取该 default 与
类型/initializer 固有要求中更强的一方，形成最终 contract。MoonIR 保留这些可验证的
per-binding contract，但绝不包含 usage-scope 节点或运行时操作。

### C011：Rc/Arc 迁移为容器（Confirmed, implemented）

`Rc<T>`/`Arc<T>` 迁移为普通名义库容器，通过最小 Resource/Drop protocol 表达引用
计数、clone、cleanup、allocator domain 和必要的线程安全事实。0.3 编译器删除对应
TypeKind、Parser、Sema 和 codegen 特判，不保留 0.2 intrinsic lowering。

`RC001`（Confirmed）：核心表面是普通 `Rc::new(value)`/`Arc::new(value)`；prelude 可以
提供同样是普通函数的 `rc(value)`/`arc(value)`。不增加 `rc {}`/`arc {}` 语言语法。

`RC002`（Confirmed）：Rc/Arc handle 默认 Affine，复制所有权必须显式 clone。`Weak`
是普通库容器，循环由程序使用 Weak 打破，不提供 tracing cycle collector。Drop 必须
infallible；Rc 使用非原子计数且不跨线程共享，Arc 使用原子计数，并要求 payload
满足编译器推导的线程安全 sysmeta。

实现边界（2026-08-09）：引用计数策略是可信 Core/Runtime 的库内实现，
不是 compiler Resource kind。编译器只看到普通名义 struct、`Clone`、`Drop`、Affine
handle 和 Global Luna release domain。当前语言没有跨线程传递或共享入口，因此 Arc
payload 的 thread-safety 判定尚不可观测；任何未来并发 API 在引入可达路径时必须
先加入 compiler-derived sysmeta 门，不得依赖用户 metadata。这是 `NP001` 的前置条件，
不恢复 Rc/Arc TypeKind。

## 5. 单层 MoonIR

### C012：MoonIR 是唯一且单层的后端 IR（Confirmed）

```text
source/package
    -> Lexer / Parser
    -> semantic / ownership analysis
    -> MoonIR
    -> MoonIR verification and transformation
    -> LLVM JIT/AOT
```

不引入 MoonHIR/MoonCore 两种公共或内部 IR。Moon Container 序列化的就是 sealed、
canonical MoonIR；LLVM backend 消费同一种 MoonIR。

单层 MoonIR 必须满足：

- 使用稳定 type/symbol/contract table reference，不序列化进程内 `TypePtr` 身份；
- frontend lookup、缓存和派生索引不是格式的一部分；
- composition、canonicalization 和优化直接变换同一 MoonIR；
- container emission 只接受 sealed、经过 verifier 的 canonical module；
- verifier 能只依赖 MoonIR 和 manifest 检查类型、所有权、cleanup、control、sysmeta、
  import/export 与 host/device 边界；
- JIT、AOT、Moon loader 不维护各自的语义 IR；
- 同一个 format version、serializer、parser 和 verifier 是唯一权威。

### 为什么曾考虑双层，以及为什么现在采用单层

双层建议来自一个工程风险：当前 MoonIR 是带 frontend 指针和 AST-like 节点的可信
进程内结构，而不可信容器需要 canonical、可序列化、可独立验证的表示。分层可以避免
立刻把编译器友好的 IR 冻结成 wire format。

但双层也会引入两套节点、lowering、verifier、printer、测试和长期同步成本。Luna 当前
尚未正式发布 Moon 格式，正适合直接重构现有 MoonIR，使它同时成为编译器 IR 和容器
格式。因此单层方案更符合轻量与纯粹目标，前提是完成上述 pointer-free、sealed 和
独立验证要求。构建中的暂态对象可以存在于 builder 中，但不得形成第二种语义 IR。

`M001`（Confirmed）：canonical MoonIR 以 CFG basic block 作为唯一执行语义，并使用
同一 IR 内的显式 lexical region/scope/cleanup table 表达 continuation 和资源边界。
region table 不是第二套控制 IR，也不得拥有与 CFG 竞争的执行语义。

`M001-A`（Confirmed）：0.3 首版 canonical CFG 采用 typed-local form，而不要求 MoonIR
本身成为 SSA。参数、源码 binding 与 lowering 生成的临时量统一由稳定 `LocalId` 引用；
名称只保留用于诊断。LLVM backend 可以继续通过 mem2reg 和后续优化获得 SSA 性能，Moon
格式不承担 phi 构造与两套 value discipline 的维护成本。

`M001-B`（Confirmed）：一个 executable body 只包含一张 CFG block table，以及 region、
scope、local、cleanup table。`BlockId`、`RegionId`、`ScopeId`、`LocalId`、`CleanupId`
都是对应规范表的零基索引，sealed 后不得重排或留下悬空引用。block 只含无控制转移的
operation 和恰好一个 terminator；jump、conditional branch、switch、return、resume、
abort 与 unreachable 覆盖 0.3 的控制出口。`if`/`match`/loop、`?`、block expression 和
slot/fragment continuation 必须在 sealing 前正规化为这些 block/edge，不能作为嵌套执行
节点进入容器。lambda 拥有独立 CFG body，不在外层 expression 中嵌入另一种 body 语义。

`M001-C`（Confirmed）：region 只记录 function/lambda/fragment/continuation/lexical/loop/
match-arm/apply 的结构归属与入口，不决定执行；successor edge 才是唯一控制语义。
scope table 记录词法 parent、所属 region 及其 local/cleanup 集合。cleanup table 使用稳定
`PlaceRef { root: LocalId, projections } + TypeRef + CleanupAction` 描述可能义务；field、
constant/dynamic index 与 dereference projection 均不得退回源码字符串。每条离开 scope
的 edge 按实际执行顺序显式列出仍 active 的 cleanup references。verifier 结合 scope parent
链与 CFG 中的初始化、move、显式 free/transfer 状态，独立重算应退出 scope 的 active
cleanup 及其逆声明序，拒绝遗漏、重复、越域、已 move place 和错误 action。普通
fallthrough、return、`?` failure、fragment abort/resume 均使用同一 edge-cleanup 规则。

`M001-D`（Confirmed）：迁移完成后的 `FunctionDecl`/`FragmentDecl`/lambda 只能拥有 canonical
CFG body，不保留 structured-body fallback 或格式兼容开关。构建期 builder 可以暂存源码
结构，但 module sealing 必须原子删除它；verifier 和 codegen 只接受 CFG。

`M002`（Confirmed）：0.3 Moon Container 只接受完全实例化的 MoonIR；generic recipe
留在编译器输入侧，不进入首版不可信容器格式。

`M003`（Confirmed）：0.3 MVP 的 Moon Container 是 host-specific，manifest 必须声明
target triple 和 data layout。跨目标 portable container 与 target-specific device code
留给后续格式版本。

`M004`（Confirmed）：容器采用确定性的 sectioned binary，显式记录 format version、
section length 和解析资源上限。manifest、type、symbol、contract、code、import/export
和必要 sysmeta 是必需 section；debug/source/device data 是可选 section。内容摘要覆盖
规范化的非签名 section，parser/verifier 必须有 fuzz corpus。

`TBD-M005`：冻结 binary magic、整数编码、section number、alignment、压缩和签名算法
等线格式细节；不得改变 M001-M004 的语义边界。

## 6. 运行时验证与进化

### C013：验证不进入普通调用热路径（Confirmed principle）

Moon 验证、Native 证明检查和 ContractId/ABI 匹配发生在安装、装载或 binding 建立
阶段。成功绑定后，调用路径只允许直接调用或已声明的最小间接分派；不得逐次重新验证
类型、所有权或容器签名。

`V001`（Confirmed）：结构验证缓存以 content digest、verifier version、target 和验证
policy 为 key。import/ContractId/ABI 与 generation 的匹配在 binding 建立时检查并单独
缓存；普通调用不复核。任一 key 成分、trust 或 dependency generation 改变即失效。

### C014：MoonRuntime 承担进化（Confirmed direction）

MoonRuntime 使用 Moon Container 和可信 Luna Native 建立新实现、装载新代码并管理
进化。安全更新至少需要 module/content/generation identity、staging、验证、解析、原子
activation、旧 generation lifetime 和失败回退。

`EV001`（Confirmed）：0.3.0 的最小进化闭环限定为 host-only、无持久状态、显式安全点
和原子 activation。staging generation 必须先完成验证、解析、binding 和 initializer；
失败时旧 generation 不变。已有 reference 固定并保留旧 generation，0.3 不回收代码。

`EV002`（Confirmed）：runtime 区分 pinned-generation reference 与 Runtime 管理的
switchable binding；普通 reference 不会因 activation 静默改指向，只有显式声明的
binding 参与原子切换。

`EV003`（Confirmed）：0.3 不支持 state migration。module initializer 在 staging 中
执行且允许在 activation 前失败；rollback 的首版含义是继续或恢复旧 generation，
不是逆向迁移已改变的外部状态。

`TBD-EV004`：冻结 pinned reference、switchable binding、initializer 和 activation 的
具体源码/API 拼写。

## 7. Symbol Query 与 Slot/Fragment

以下方向已经确认：

- `Q001`（Confirmed）：统一 Symbol Catalog；compile-time query 产生在 MoonIR 前解析并
  擦除的 typed set；
- `Q002`（Confirmed）：runtime query 只能返回显式 runtime-exported、descriptor-backed
  typed reference；
- `Q003`（Confirmed）：查询必须使用 `.one()`、`.optional()` 或 `.all()` 等显式 terminal
  决定 cardinality；no-match/ambiguous 不由隐式链接顺序处理；
- `SF001`（Confirmed）：slot 是模块级二等 Symbol，拥有稳定 SlotId/ContractId；调用是
  局部 control operation，不产生可传递的 slot value；
- `SF002`（Confirmed）：fragment 名义绑定目标 SlotId，继续服务于受限控制流
  composition，不成为通用 service/provider；
- `SF003`（Confirmed）：静态 apply composition 前移到 MoonIR，保持零 Runtime 成本；
- `SF004`（Confirmed）：普通函数/函数引用与 RuntimeFragmentRef 分开建模，形状相同
  也不允许隐式互换。

`SF005`（Confirmed）：0.3.0 slot/fragment result 固定为 `unit`。静态路径支持
single-shot interceptor 和 single-shot context；runtime 首版只支持 single-shot
interceptor。non-unit result、`many` 和 runtime context/continuation ABI 延后。

`TBD-Q004`：冻结 `.all()` 的规范排序以及显式排序 API；结果不得依赖链接或注册顺序。

`TBD-SF006`：冻结 module-level declaration、名义 fragment target 和 lexical invocation
的具体语法，以及 return、abort、`?` 和 cleanup 在 single-shot context 中的精确交互。

Luna 不因为 Slot/Fragment 借鉴代数效应的控制思想而引入 effect 机制。

## 8. 迁移摘要

迁移说明记录破坏性变化，但 0.3 编译器不实现旧语义兼容。

| 领域 | 迁移前 0.2.1 | 迁移后 0.3 | 理由 | 实现方式 |
|---|---|---|---|---|
| 兼容 | 单一 0.2 语义 | 单一 0.3 语义 | 保持编译器轻量纯粹 | 删除旧逻辑；旧源码使用旧编译器 |
| 阶段 | CompileTime/Runtime/Dynamic | Compile-time/Runtime | Dynamic 混合多个概念 | 删除 Dynamic retention 和分支 |
| effect | 无正式 effect；facts 分散 | 仍无 effect；统一只读 sysmeta | 不扩大语言机制 | compiler-derived schema + verifier |
| 类型默认 | 具名默认结构 | 具名默认名义 | 稳定生态和 runtime identity | 单一新 TypeId 规则 |
| Usage | 单 binding 修饰 | 增加 affine/linear block 糖 | 降低资源代码噪声 | Sema 固化后擦除 block policy |
| Rc/Arc | compiler-special TypeKind | 普通名义容器 | 移除特判 | Resource/Drop protocol + 库实现 |
| MoonIR | pointer-heavy AST-like 内存 IR | 单一 canonical、serializable IR | 支撑容器同时避免双 IR | 原地重构 type/symbol reference |
| Native | 通用 descriptor/plugin ABI | 可信 Luna Native | 快速装载且来源可验证 | 代码摘要和构建证明 |
| Moon | 未实现容器 | 本地验证的安全容器 | 安全接入和进化 | serializer/parser/verifier/loader |
| C FFI | 显式 extern C | 继续以 extern C 标记 foreign 边界；无通用 unsafe block | 外部实现不可由 Luna 证明 | `-t cffi` 产物 + extern C 声明 |
| 旧语法 | 编译器接受 | 0.3 编译器不接受 | 不维护兼容分支 | 文档迁移表/独立迁移工具 |

## 9. 实现优先级

### C015：首个代码实施工作是拆分 Sema（Confirmed）

0.3 的首个代码实施工作是在不改变语言语义的前提下拆分当前 `SemanticAnalyzer`。
拆分期间不得顺便实现其他 `TBD-*`、改变 AST/MoonIR/ABI，或改变诊断与 codegen 结果。
对 tooling 继续保留一个 `SemanticAnalyzer` facade，避免把内部结构扩散为公共 API。

建议采用一个共享 `SemanticContext` 和五个粗粒度职责组件，而不是制造大量微型 pass：

1. `DeclarationCollector`：声明身份、命名空间、metadata schema、FFI declaration；
2. `TypeResolver`：TypeAST、constraint、trait/impl coherence、generic instantiation；
3. `BodyAnalyzer`：statement/expression/call/iterator/device 语义；
4. `CompileTimeEvaluator`：const、constraint evaluation、selector，未来承接 Symbol Query；
5. `ControlAnalyzer`：Slot/Fragment/apply 的控制语义。

现有 `OwnershipChecker` 保持独立；不创建 EffectAnalyzer。`SemanticContext` 统一持有 symbol、
type/trait catalog、推导状态、诊断和 declaration reference，组件不得复制权威表。

`SEMA001`（Confirmed）：依赖方向为 facade -> components -> context/core；组件之间通过
窄接口协作且不相互持有。首个重构提交前只需把该边界落成具体 C++ API，不再重新决定
职责划分或创建第二套 catalog。

实施完成记录（2026-08-09）：facade 与唯一持有状态的 context 已落地；
`BodyAnalyzer`、`DeclarationCollector`、`TypeResolver`、`ControlAnalyzer` 和
`CompileTimeEvaluator` 已在语义等价前提下抽取。每个组件现在接收独立的 context
capability，其中只包含经审计的引用和服务；五个 analyzer 不再是 `SemanticContext`
的 friend。没有复制权威 catalog，也不存在组件之间的持有关系。因此 C015 与
`SEMA001` 已通过实现完成门。

计划门完成记录（2026-08-09）：当时的五项未决登记、已确认 `T003` 与规范术语是穷尽
集合，并由 `luna.0.3-design-contract` 守护。最终 0.2 迁移 corpus 包含十个代表性
案例，固定到上述检查点，并已由该编译器独立重放。因此优先级第 2、3、4 项已经通过
完成门；下一项实现工作是第 5 项：在不改变当前 codegen 的前提下引入 shadow identity
与 sysmeta。

shadow identity 实施完成记录（2026-08-09）：TypeId、ShapeId、SymbolId、ContractId 与
AbiLayoutId 已成为互不混用的 C++ 类型。canonical MoonIR type record 现在携带可重算的
Type/Shape/ABI-layout payload，declaration record 携带可重算的 Symbol/Contract payload；
sysmeta schema 1.2 首次增加封闭的 identity namespace 来投影这些 ID。verifier 会拒绝 payload
不一致和不同 payload 的 hash collision；确定性测试证明相同 contract 共享 ContractId，
不同 declaration 保持不同 SymbolId。LLVM codegen 继续消费原字段，完整 JIT/AOT suite
没有变化。因此优先级第 5 项已通过完成门。

名义默认实施完成记录（2026-08-09）：所有具名 struct/enum 现在都获得声明
TypeId；旧的源码默认结构分支与冗余的 `nominal` modifier/keyword 已删除，
且没有增加 language/edition 开关。匿名
`{ field: Type }` 和 `{ field: value }` record 是内联结构 aggregate；
`Target { field: value }` 按显式具名构造检查，字段求值顺序与规范 identity/layout
顺序独立。C++ 风格 `<Constraint T>`、具名 `where Constraint<T>` 和 inline
`where predicate` 都使用已有编译期 constraint evaluator；inline 写法没有 symbol，也没有
MoonIR node。正例、负例、package 限定名、reflection、identity、layout、MoonIR verifier、JIT 与 AOT
证据已通过全部 48 项注册测试。因此优先级第 6 项已通过完成门。具名 product 仍使用
当前指针表示这一实现细节；本阶段不声称已完成具名 product 内联布局重设计。下一项实现工作是
第 7 项：usage block 糖与最终 binding contract。

usage block 实施完成记录（2026-08-09）：`affine {}` / `linear {}` 现会把词法 default
传给 local、enum pattern 与 `for` binding；普通 block 继承，嵌套 usage block 覆盖，lambda body
从 Copy 重新开始。前置 `copy let` / `affine let` / `linear let` contract 替换 block
default；显式 contract 若弱化类型/Resource、moved source 或 function result 的要求，
Sema 会拒绝。Copy 类型的裸 allocation 不会被隐式提升为 affine；Luna 保留显式安全、
类 C 的默认语义。frontend 只把最终 per-binding usage 记入可验证 MoonIR；usage-scope construct
和运行时操作均不进入 lowering。嵌套/覆盖/binder/lambda 正例、linearity 与 weakening 反例、
JIT/AOT 行为及全部 48 项测试均通过。因此优先级第 7 项已通过完成门。下一项是第 8 项：
完成通用 Resource/Drop contract 与递归 cleanup 路径。

Resource/Drop 实施完成记录（2026-08-09）：`Drop` 现直接解析为规范
`org.luna.core::resource::Drop` identity，不存在 legacy language 分支。编译器为每个冻结
类型派生一个 typed ResourceContract，其中包含 relation、usage、cleanup strategy、lifetime、
management/release domain 与 recursive-cleanup facts；sysmeta schema 1.3 和 MoonIR verifier 会携带并验证该
contract。源码 `Drop::drop(&mut T) -> unit` 是不可失败的就地 finalizer：其后由编译器递归
清理仍拥有的字段，最后才按实际 allocation strategy 释放外层 storage。因此具名 product、
匿名 record、array、Result/enum 的活动 payload、shared payload 与泛型名义实例统一使用
同一递归 cleanup 实现。泛型 Drop impl 的 type parameter 按 impl parameter 解析；在 Drop
body 尚未 monomorphize 前，其 target layout 必须 representation-stable，inline
type-parameter-dependent storage 会被拒绝。Drop contract 在普通 body 之前注册，所以语义与声明顺序无关。具名 product storage 的字段
offset 现按实际 value size/alignment 计算，包括 pointer-represented nominal field。递归、
泛型、声明顺序正例以及 signature/Copy 弱化反例均通过 JIT/AOT 与完整测试。因此优先级
第 8 项已通过完成门；下一项是第 9 项：实现普通 Core `Rc`/`Arc` 容器并删除其 compiler
special kind。

Rc/Arc 容器实施完成记录（2026-08-09）：`Rc<T>`/`Arc<T>` 现由
`org.luna.core` 声明为普通泛型名义 struct，`Rc::new`/`Arc::new`、prelude
`rc`/`arc` 与 `resource::Clone` 都是普通库函数/trait。共享单元使用 Runtime
ABI v1 分配、非原子/atomic retain-release 以及 compiler 生成的 type-erased Drop
callback，最后一个 handle 精确一次清理 payload。parser token、TypeKind、Sema/
codegen cleanup、`clone` intrinsic 与旧 Runtime 入口已删除；`rc new`/`arc new` 只在
冻结的 0.2 corpus 中由旧编译器重放，0.3 明确拒绝。JIT/AOT、嵌套 Drop、
显式 clone、隐式复制拒绝、MoonIR 普通 nominal 与 LLVM Runtime ABI 证据均通过；
完整 51 项 CTest、strict-warning 构建与资源路径 ASan/UBSan 均通过。
当前具名 product 仍为指针表示，因而 handle wrapper 的 inline/transparent ABI 优化
不在本次语义迁移中假定完成。因此优先级第 9 项已通过完成门；下一项是
第 10 项：原地将现有 MoonIR 重构为 table-referenced canonical IR。

第 10 项的前两个子阶段已经完成。类型子阶段使 sealed MoonIR 的 metadata、声明、签名及操作节点只保存
`TypeId`，完整类型结构被冻结在唯一 type table 中；LLVM backend 使用格式外、可丢弃的
materializer 从该表按 `TypeId` 重建并缓存后端类型，不保留或读取 frontend `TypePtr`。
MoonIR verifier 同样只从冻结记录重建类型，并重算 TypeId、ShapeId、layout 及全部结构引用，
所以该变化没有程序运行时成本，成本只发生在编译/装载验证阶段。迁移同时修复了两个原先
被进程内指针身份掩盖的身份缺陷：Iterator recipe 的 source 参数现在进入稳定 shape/type
identity；seal 会合并 nominal forward placeholder，再从闭合的冻结类型图规范化 ShapeId
和 layout，不再接受任意 frontend placeholder 的临时字段图。独立 materialization、
注册顺序确定性、篡改拒绝及完整 JIT/AOT 回归已有测试证据。

symbol/contract 子阶段使直接调用、函数值、trait impl、Iterator/FromIterator
协议、kernel、`From` 转换、dynamic select 和 fragment binding 统一保存
`DeclarationRef { SymbolId, ContractId }`。linkage name 只是 declaration table payload；verifier
检查使用点的 SymbolId 存在、声明 kind 正确且 ContractId 匹配，backend 在验证后
才由声明表解析 linkage。Drop glue 也使用该稳定引用，不再进入 sealed IR
作为裸字符串符号。没有源码声明的 compiler-owned `Drop`/`From` trait 在被引用时
会获得普通的合成声明表行，因而不需要 verifier 特殊旁路。这些匹配仍只发生在
编译/装载或 binding 建立时，不进入普通调用热路径。

M001 子阶段现已完成 table 与构造基础。稳定的 block、region、scope、local、cleanup
及 projected-place 引用共同定义单一 typed-local CFG；结构 verifier 检查 table 归属、
terminator 形状、词法可见性、local 定义、switch binding，以及跨边的路径敏感 cleanup
状态。construction-only builder 会消费临时 structured body，并为普通语句、词法块、
`if`/`else`、`while`、`match` 和 protocol-backed `for` 产生该 CFG。直接 `Iterator`
与隐式 `IntoIterator` 都会成为普通 call 加 `Switch`/backedge；转换所得 hidden state
只初始化一次，并在 `None` 退出边清理。compiler-fused range 和 Copy-array recipe（包括
shared/mutable/consuming source mode 与 `take`）会展开为普通 source/index/limit/counter
local、索引取 item、比较、赋值、branch 和 backedge；没有新增 iterator terminator 或不透明
recipe operation，verifier 会拒绝 sealed CFG 中未展开的 recipe。builder 不会与旧 body
并列挂接：sealed executable 在任何阶段都不能同时具有两套执行含义。

无捕获 lambda 的子阶段也已完成：lambda expression 仅作为闭包值节点，其
structured body 在构造时被消费为一份独立、以 `Lambda` region 为根的
canonical CFG。这不是双层 IR：父函数与 lambda 分别只有一张 CFG，闭包值
仅引用 lambda 的可执行体。verifier 会递归验证子图，核对闭包类型、参数
contract 与 parameter local，并拒绝 body/CFG 并存。由于当前 Sema 原本就在
closure environment layout 实现前拒绝局部捕获，该子阶段同样明确拒绝
`captures`，没有引入隐式或不完整的闭包 ABI。签名一致性检查同时暴露并
修复了 `linear T`/`affine T` 显式 lambda 返回类型绕过已解析 nominal
identity 的 lowering 缺陷；usage wrapper 现在仍只是 binding contract，内层
`T` 的 TypeId 不再被退化为同名 placeholder。

基于该 lambda CFG，Copy item 与 Copy callable contract 的无捕获
`map`/`filter` 现也已进入同一 canonical 展开。source 与每个 callable/`take`
参数按源码顺序在 loop init
求值一次；`map` 成为普通 typed-local call 及结果 local，`filter` 成为 bool
call 与 branch，拒绝边直接进入统一 index increment/backedge。因此
`filter`/`take` 的先后顺序会决定哪些元素消耗 counter，不建立中间容器、
不新增 iterator IR operation。verifier 额外核对 local callable 的参数/结果
类型及 let initializer/local 类型，所以展开后不再依赖 recipe 上的前端信任。

第一个 non-Copy 逐元素切片已完成，范围是在直接 `for` 中由 `map` 把 Copy 输入
变换为 owned Affine 值。该结果先初始化一个 synthetic Affine local；后续 `filter`
predicate 通过显式 `SharedBorrow` 读取它，`take` 则保持该值不变。拒绝或耗尽边在离开
逐轮 scope 前释放 temporary；接受的 item 可以显式 move 给后续 owning `map`；该 map
消费旧 synthetic local，并在普通
结果 `LetStmt` 中原子激活 Copy 或 Affine replacement。adapter chain 结束时，最终 item 再显式
move 到源码可见的迭代 binding。普通 body fallthrough 在现有 cleanup edge 上释放该 binding，
early return 使用现有 return cleanup set。verifier 会独立拒绝复制 map input/result，以及任一
缺失的 rejection/body-exit cleanup，因而不增加 runtime initialized bit。这也修正了底层
relation 规则：borrowed local 使用 Copy cardinality，即使底层类型是 Affine 也不拥有 cleanup。
move-only consuming source array、Linear 逐元素状态、non-Copy callable 或 closure environment、materialized recipe
状态与 iterator terminal 仍不在该切片内。

下一个 consuming-source 切片所需的 guarded-array cleanup 基础现已冻结。canonical CFG
不沿用 structured backend 的 `[N x i1]` initialization bitmap，而是记录一个 synthetic Copy
整数 `nextUnread` cursor，并为每个 array element 记录一行 constant-index cleanup。仅当
`nextUnread <= i` 时才清理元素 `i`；因此 unread tail 只需一个 runtime word，guard
检查也只出现在 cleanup exit。verifier 强制要求 owned frozen array、同一 scope 的
synthetic 整数 cursor、每个元素恰好一个 guard、所有 guard 共用一个 cursor，且禁止与
whole-array 或 unguarded cleanup 混用。cleanup table 现也会对 projected place 做确定性排序。
这一步只提交可验证的状态表示，builder 仍未放宽 move-only consuming-array 拒绝；
cursor update 与所有 early-exit edge 必须在下一切片同时生成。


slice bound 子阶段也已完成。slice recipe 的 source 只求值一次，运行时上界只在
loop init 物化一次，并与 shared array recipe 一样使用普通的索引借用 CFG。上界使用
`SliceLengthExpr` 表示；它是 slice 的基本投影，不是 iterator operation 或 terminator。
该投影保留 slice ABI 的 `usize` 长度，没有把旧 backend 缩窄为 `i32` 的行为固化进
canonical IR；slice 循环索引因此也使用 `usize`，而现有源语言 `take` 计数仍为 `i32`。
verifier 独立要求操作数是冻结 slice、结果是 `usize`。由于当前稳定语言表面只有只读 slice，
Sema 与 canonical 构造都接受直接/shared slice 迭代，并拒绝 mutable 或 consuming slice recipe。


materialized recipe 的第一个子阶段已完成 `for` 消费路径。绑定 range、借用 array/slice 或
consuming Copy array 时，现在会立即产生普通 source/index/limit/adapter locals，compiler-domain
Iterator binding 本身会被擦除。consuming Copy array 在绑定点建立快照，source 与 adapter 参数
保留原有的从左到右求值顺序。既有 index local 被强化为 affine synthetic local，并在消费时
move 进循环，因而它同时成为零额外状态的单次消费凭证。第二次消费、复制该 cursor 或路径间
消费不一致都会被 canonical CFG ownership dataflow 拒绝。sealed graph 中不保留 Iterator-typed local、recipe metadata、
runtime token、allocation 或新 ABI。

首个 materialized terminal 子阶段也已完成。`count` 与 Copy accumulator `fold` 会从已擦除的
recipe 状态继续展开为带外层结果 local 的普通循环，而 expression-statement `for_each`
会生成普通的循环体 call。在 terminal 上追加的 adapter 会在 terminal 参数之前求值一次。
该子阶段接受作为直接 initializer/return value 的有值 terminal，以及直接普通 call
的唯一参数；`for_each` 只接受 expression statement 位置。更广泛的 expression sibling
hoisting 在该子阶段被明确延后，因而当时不会猜测或重排求值顺序；后文记录了其首个
eager Copy 覆盖。

materialized `collect` 子阶段也已完成。构造器首先核对三个冻结的
`FromIterator` declaration 签名，再把 `begin()` 降低为唯一的 synthetic affine
builder local。每次循环体 `push` 都显式取该 local 的 mutable borrow，所有正常 recipe
退出路径都在 `finish(move builder)` 前汇合。affine finish 结果先进入第二个
synthetic local，再显式 move 给源码中的 initializer、return 或直接 call consumer。
因此既有 cleanup dataflow 会各自只激活一次 builder/result obligation，并在 transfer 时
注销；不需要 runtime iterator object、ownership flag 或新 ABI 状态。独立 verifier 现在也会
核对 declaration-backed call 签名（包括显式 borrow argument 的 relation），而 synthetic
ownership dataflow 要求 finish transfer；伪造的 shared builder borrow 或 finish 处的拷贝都会被拒绝。

直接 non-materialized Copy-terminal 子阶段也已完成。在直接 initializer/return 位置，
或作为直接普通 call 的唯一参数时，receiver 的 source/start、bound 与 adapter argument
会按源码顺序在 terminal argument 之前物化，随后普通状态进入上述同一 terminal 展开。
这覆盖直接 `count`、Copy `fold`、expression-statement `for_each` 和 affine-builder `collect`，
不增加第二套 lowering。该直接切片最初仍拒绝位于更早 sibling operand 之后的 terminal；
下面的 Copy operand-hoisting 子阶段现已取代这一临时边界。

affine-accumulator `fold` 子阶段现在也已完成。一个 synthetic affine local 持有初值，
每轮都被 move 给 reducer，并在同一次 transfer assignment 中由 reducer 的 affine 返回值
重新初始化。独立 ownership dataflow 会拒绝复制 accumulator、未先消费就替换，或复制最终
结果；源码 consumer 接收显式 move。普通循环回边与零次迭代出口因此都保持唯一 active
cleanup obligation，不增加 initialized bit、runtime ownership flag 或第二个 accumulator。
linear accumulator 仍不进入这种隐藏 terminal state。

eager-expression 的有序 operand 子阶段现已完成。普通 call 的
callee/argument、非短路 binary、index、array、variant、dynamic-select filter 与 launch
operand 都会按源码顺序扫描；一旦后续 operand 展开为 iterator terminal CFG，较早的
普通 Copy 值会先进入 synthetic local，原表达式再通过该 LocalId 读取。
非平凡的较早 `unit` 表达式则会作为普通 `ExprStmt` 恰好执行一次，父表达式中用
零大小 `UnitExpr` 占位，因而无需 synthetic local 或任何运行时状态。因此
callee load 或有副作用的 earlier call 不会被推迟到 terminal 循环之后，也不增加运行时
tag/ownership flag。独立 verifier 会检查这些 let/local/type 引用。同一有序 lowering
现也允许由 Affine-returning call 或显式 `move` 生成的无 cleanup Affine 值：其
synthetic local 使用 Affine contract，父表达式通过唯一的生成 `MoveExpr` 读取，
verifier 会拒绝伪造的 Copy 读取。带 cleanup 义务且显式转移的 Affine 值，
现也可以跨越后续控制流。构建 bridge 会在父表达式尚未发射时记录其
synthetic cleanup row 为 active；在此期间构建的 `TryExpr` 失败路径或结构化
`return` 会把它纳入普通 `Return.exitCleanups`，成功路径则到达父表达式中
生成的 `MoveExpr` 并消费它。独立 ownership dataflow 会拒绝缺失的 early-exit
cleanup，因而不需要 runtime initialized flag。显式转移的 Linear sibling 只在
递归扫描证明余下 operand 不含可在父表达式消费它之前退出的 `TryExpr`、
`BlockExpr` 或 `IfExpr` 时才可 hoist。其 synthetic Linear local 由 verifier 的
纯编译期 ownership marker 跟踪，并通过唯一的生成 `MoveExpr` 读取；与 Affine
不同，Linear 在 early exit 上没有 cleanup 退路。跨越潜在 early exit 的 Linear
sibling 仍被明确拒绝，因为它违反恰好一次消费约束；它不会被隐式复制或重排。
短路 operand 则进入下文的 conditional CFG 规范化。

匿名 structural record 现已与会分配的 product 区分。其 inline 值构造没有 allocation
boundary，因而字段表达式使用与 array、variant 相同的源码有序 operand 规范化；
terminal 字段会在最终 `RecordLiteralExpr` 中成为普通 result-local 引用。

面向 allocation 的构建子阶段现已完成 unique named-struct 与显式 heap allocation。
`AllocateStmt` 首先定义一个 owned Affine allocation LocalId；其
`CleanupKind::Allocation` row 即使在存储类型本身是 Copy 时也会释放 backing storage。
initializer 随后通过普通的 operand 规范化按源码顺序求值。必须跨越后续控制流的值
保存在 synthetic local 中；提前的 `?` 或结构化 return 会逆序清理这些已求值的值，
然后释放 raw allocation。只有当所有 initializer 都成功后，`InitAllocationExpr` 才消费
raw identity，并把各值转移到冻结的字段序号中；binding 随后只持有一个最终 value
或 allocation cleanup。因而部分初始化无需 runtime initialized bit，也绝不会对未初始化字段
执行 Drop。独立 verifier 会拒绝缺失或种类错误的 raw cleanup、不存在的 allocation
LocalId、layout/type 不匹配，以及 sealed graph 中任何遗留的 `HeapAllocExpr` 或会分配的
`RecordLiteralExpr`。丢弃 owning allocation result 会被拒绝而非泄漏。其他 storage kind 仍不在
当前唯一的 `Unique` storage model 中。

当前 Luna block 语法的第一个 conditional-expression 切片也已完成。由于 block 尚无
tail value，`BlockExpr` 和 block-style `IfExpr` 在语义上都是 `unit`；CFG 构建会消费
其 structured block，产生 lexical region 和普通 branch/jump edge，然后用零大小
`UnitExpr` 替换已完成的表达式。嵌套 `else if` 和位于有序 operand 位置的 `if`
使用同一路径。`UnitExpr` 不持有 body 或 successor，因而不会引入第二层控制 IR。
verifier 仍会拒绝 sealed graph 中的任何 `BlockExpr` 或 `IfExpr`。若未来设计 tail value，
则必须显式引入 synthetic result local；0.3 实现不会自行推定这种值语义。

`TryExpr` 规范化作为下一个 conditional 切片也已完成。operand 只求值一次，并成为
基于冻结 Result ABI（`Err = 0`、`Ok = 1`）的普通 `Switch`。成功 payload 是唯一继续
路径上的 pattern local。失败路径会按需调用静态解析的 owned `From` witness，构造普通
`ResultConstructExpr`，在 return edge 上执行源码推导的 cleanup 集合，并以 `Return`
终止。move-only Result/error payload 使用显式 transfer，因而独立 ownership dataflow 可以
验证 operand 消费、转换、传播返回和 cleanup，不需要 exception runtime 或隐藏 success flag。
`ResultConstructExpr` 只是数据；sealed CFG 仍拒绝任何残留 `TryExpr`。

短路表达式规范化现已完成。`&&` 与 `||` 各使用一个普通 synthetic Copy `bool`
local，分别初始化为 `false` 与 `true`，再在短路路径上跳过右 operand。只有
必需路径会求值并写入右 operand，随后两条继续路径合流。右 operand 内部的
嵌套控制流也只在该 conditional path 上规范化，因而没有操作会跨过语言的
求值边界被 hoist。这不增加 runtime ownership flag 或第二套控制表示；状态只是
普通布尔值，独立 verifier 会拒绝 sealed CFG 中任何残留的短路 `BinaryExpr`。

只求值一次的语句判别值现在也使用同一表达式规范化。`if` condition 会在
`Branch` 之前完整规范化，`match` scrutinee 会在 `Switch` 之前完整规范化；
iterator terminal 和嵌套 conditional expression 因而不会残留在这两种
terminator operand 中，也不会被延迟到 arm 开始之后求值。

重复求值的 `while` condition 现也已规范化，且不会复用 one-shot state。属于
loop 的 header 同时是初始 edge 与 body backedge 的目标；每次经过 header 都会
进入一个子 condition-evaluation scope。terminal、短路及其他 synthetic local 只存活于
该 scope，并在 body 和 exit edge 上都变为不可见。canonical ownership dataflow
因此会在下一次进入 header 前停用 compile-time affine marker，并在下一次执行
声明时重新激活。这在不增加 runtime initialized flag 的情况下表达了源语言的
重复求值；普通 condition 仍保持原有的紧凑单 block 形态。

第一个 slot/fragment 子阶段现已完成 static single-shot interceptor。带显式词法体的
`apply { ... }` 在构建期解析其 `DeclarationRef` 并形成 `Apply` region；每个被调用的
interceptor 由 construction body 克隆进 `Fragment` region，slot body 则形成同一 Apply 下的
`Continuation` region。interceptor 正常结束以经验证的普通 `Jump` 进入 continuation
entry；`Resume` 只保留给 context 的显式 resume point。`abort()` 以 `Abort` edge 进入 fragment exit，fragment 内的 `return` 则变为携带
cleanup 的普通 jump 进入同一 exit，因而两者都不会执行 continuation。continuation 内的
return 仍是外层函数的 `Return`。每个 Fragment region 携带稳定的 declaration/contract reference。
verifier 会独立要求自动转发只出现在 interceptor contract 下，resume 只出现在 context contract 下并进入
同一 Apply 的 continuation，abort 进入其所在 fragment 的 exit。因此 static composition 不保留 descriptor、
selector、function pointer、heap continuation 或 runtime dispatch state。声明体只由构建桥克隆，
仍可供其他静态调用点使用；sealed graph 中不会保留第二个执行语义。该边界拒绝无 block
apply，因为 0.3 要求显式词法体。

static single-shot context 现也使用同一 composition，而不是另一种 CPS operation。每个源码
`resume()` 以 `Resume` terminator 进入保持词法生命期的 `Continuation` region；continuation
正常结束后跳回该 resume 之后的语句。fragment local 在该边上保持存活，但在 continuation 内不可见；
continuation 中的名称仍绑定到调用环境。continuation 内的 `return` 是外层函数 `Return`，context
正常落尾是隐式 `Abort`，fragment 内的显式 `return`/`abort()` 则都经 fragment exit 跳过 continuation。
verifier 会拒绝伪造的 fragment identity、以普通 context jump 绕过 `Resume`、continuation 通过非 exit edge
逃逸，以及 continuation 引用 fragment-local state。该图不需要 heap continuation、runtime descriptor、dispatch 或
ownership flag。

fragment parameter 现使用携带显式最终 relation 的普通 canonical Let/local binding。Fragment region
记录其有序 LocalId，verifier 会将每个 TypeRef、SharedBorrow/MutableBorrow/Owned relation 及
Copy/Affine/Linear usage 与冻结 fragment contract 匹配。因而 borrowed binding 不拥有 cleanup、
不消费 source；owned move-only binding 仍要求已有的显式 transfer，并且只激活一个 cleanup obligation。
即使同时伪造 binding row 和 Let relation，也无法绕过 region-level contract check。这是对 typed-local
model 的扩展，不会增加 fragment-only parameter operation。

真实 frontend 到 construction bridge 现已与手工 canonical fixture 走同一路径。直接调用 slot default
时，由于源码中不存在负责容纳 Fragment/Continuation 对的 `apply` block，构建器会合成一个仅在
construction 阶段存在的 `Apply` region；它随静态 composition 一同擦除，既不是新的源码构造，也不是
runtime state。绑定 local 时，如果 identifier 的 construction form 没有冗余 expression TypeRef，构建器会
从已解析 LocalId 的冻结 TypeRef 补齐；若非空 TypeRef 与 local 冲突，则保留冲突并由独立 verifier 拒绝。
集成门禁依次执行源码 parsing 与 Sema、Luna lowering、structured verification、CFG construction 和 CFG
verification，并检查 default fragment、词法 capture、resume edge、region topology，以及 declaration
construction body 未被消费。同一源码级门禁还覆盖显式 static `apply`；另一份 dynamic composition
源码可以通过 frontend 与 Lowering，但必须在该 static CFG 边界被拒绝。multi-shot 与 runtime apply
仍属后续切片。

第 10 项尚未整体完成。捕获式 closure environment 与其余 non-Copy
item/callable 逐元素 ownership 转移仍是明确的后续边界。
跨越潜在 early exit 的 Linear hoisting 因违反恰好一次约束而保持非法，它不是延后的
lowering 功能。
随后规范化 slot 和 fragment
路径，原子替换 structured executable body，并让 backend 消费同一 CFG。只有删除
structured 执行路径且完整 verifier/codegen 回归门通过后，本项才算完成；
serializer/parser 仍属于第 11 项。

| 顺序 | 优先级 | 工作 | 完成门 |
|---:|---|---|---|
| 1 | P0 | 语义等价地拆分 Sema，并在改动前固化现有回归基线 | Semantic/MoonIR/diagnostic/codegen 证据不变；tooling facade 不变 |
| 2 | P0 | 冻结本文 Confirmed/TBD 边界和术语 | 未确认内容没有被实现假设替代 |
| 3 | P0 | 建立 0.2 最终基线、迁移 corpus 和破坏性变化清单 | 0.2 证据可由旧编译器独立复现 |
| 4 | P0 | 冻结 `-t` 后缀、package-kind 拼写和 foreign API 细节 | CLI/产物 RFC 完成 |
| 5 | P1 | 引入 shadow SymbolId/ContractId/AbiLayoutId 与 sysmeta schema | 不改变当前 codegen，identity 可比对 |
| 6 | P1 | 一次性切换 0.3 名义默认并删除旧结构默认路径 | 无 language/edition 分支 |
| 7 | P1 | 实现 usage block 糖及最终 binding contract | MoonIR 不出现 usage-scope 节点 |
| 8 | P1 | 完成通用 Resource/Drop contract | 递归 cleanup 正负路径闭合 |
| 9 | P2 | 实现 Rc/Arc 库容器并删除 compiler special kind | JIT/AOT/cleanup parity 通过 |
| 10 | P2 | 将现有 MoonIR 原地重构为 table-referenced canonical IR | 无 frontend pointer identity 进入 sealed IR |
| 11 | P2 | 完成单一 MoonIR verifier、serializer、parser 和 fuzz corpus | deterministic round-trip；非法容器拒绝 |
| 12 | P2 | 实现 `-t` 的 Moon/Native/CFFI 产物路径 | 三种信任边界不会互相自动降级 |
| 13 | P3 | 完成 compile-time Symbol Catalog/query 和静态 composition | 静态结果擦除且无 Runtime 成本 |
| 14 | P3 | 完成 Runtime descriptor、typed reference 与 loader | Moon/Native load-once 闭环 |
| 15 | P4 | 完成最小 generation staging/activation/rollback | 已确认的最小进化闭环通过 |
| 16 | P4 | 全仓审计 Dynamic、旧 Rc/Arc、旧 slot/plugin 等遗留表面 | 各新实现已原子删除对应旧路径；生产代码无 0.2 compatibility branch |
| 17 | P5 | 同步 formatter、LSP、Lunax、文档、benchmark 和发布门 | 生态只面向 0.3 新语义 |

每个阶段必须同时增加正例、负例、MoonIR/ABI 证据和按需付费检查。回滚依赖版本控制，
不通过在生产编译器中永久保留旧路径实现。

## 10. 非优先目标占位

以下延后决定已经确认；它们不进入 0.3.0 优先实现范围：

- `NP001`（Confirmed deferral）：并行计算和通用并发；
- `NP002`（Confirmed deferral）：扩展 GPU 数据类型、grid 和跨 generation device update；
- `NP003`（Confirmed deferral）：stateful hot migration；
- `NP004`（Confirmed deferral）：runtime context/multi-shot continuation ABI；
- `NP005`（Confirmed deferral）：hotspot JIT、PGO、deoptimization 和 code reclamation；
- `NP006`（Confirmed deferral）：开放式 runtime reflection 和通用 runtime trait object。
