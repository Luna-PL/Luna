# Luna 0.3 总体设计草案

[English](luna_0.3_design.md) | 简体中文

> 文档类别：RFC / 总体设计
> 适用版本：候选 Luna 0.3.0
> 状态：Draft
> 规范性：非规范；本文记录已确认方向与待决占位，不改变 0.2.1 当前契约
> 实现核对：设计记录，基于 `bf8d73e`（2026-08-09）

本文是 Luna 0.3 的总纲。现有
[Slot/Fragment 重构审计](luna_0.3_evolution_audit.zh-CN.md)是受本文约束的专题审计，
不是 0.3 的总规范。

文中使用以下状态：

- **Confirmed**：项目负责人已经明确确认的方向；
- **Proposed**：有具体建议，但尚未冻结；
- **TBD-xxx**：必须在实现对应能力前决定的稳定占位。

所有计划语法都只是 Draft。实现、测试、参考文档和变更日志同步完成前，本文不声明
编译器已经支持这些能力。

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

`TBD-T003`：冻结三个目标的文件后缀、manifest package-kind 拼写和平台链接细节；这些
是产物格式细节，不改变上述语言与信任语义。

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
- 0.3 不保留 0.2 默认结构语义的编译模式。

`TY001`（Confirmed）：匿名 record/tuple/function shape 保持结构化；具名类型即使 layout
相同也不发生隐式结构转换。shape constraint 可以检查结构关系，但不会抹除 TypeId；
具名类型之间必须显式构造或投影。

`TBD-TY002`：冻结匿名 record、shape constraint 和显式构造/投影的具体源码拼写。

### C009：relation 与 usage 正交（Confirmed）

Ownership relation 继续表示 owned/shared borrow/mutable borrow，usage 继续表示
Copy/Affine/Linear。`affine`/`linear` 修饰 binding contract，不改变 TypeId。

### C010：`linear {}` / `affine {}`（Confirmed direction）

块状语法是纯语法糖：块内新声明变量默认使用对应 usage。Sema 固化每个 binding 的
最终 usage 后，MoonIR 不保留 usage block 节点，因此没有运行时成本。

Draft 示例：

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

`US002`（Confirmed）：普通嵌套块继承当前 usage default，嵌套 `linear {}`/
`affine {}` 覆盖它。默认适用于块内新建的 local、pattern 和 loop binding；borrow
relation 与 usage 正交，因此 borrow binding 也取得该默认值，但仍受 borrow checker
约束。lambda/局部函数参数从 Copy default 重新开始，capture 保留被捕获 binding 的
既有 contract。

### C011：Rc/Arc 迁移为容器（Confirmed direction）

`Rc<T>`/`Arc<T>` 迁移为普通名义库容器，通过最小 Resource/Drop protocol 表达引用
计数、clone、cleanup、allocator domain 和必要的线程安全事实。0.3 编译器删除对应
TypeKind、Parser、Sema 和 codegen 特判，不保留 0.2 intrinsic lowering。

`RC001`（Confirmed）：核心表面是普通 `Rc::new(value)`/`Arc::new(value)`；prelude 可以
提供同样是普通函数的 `rc(value)`/`arc(value)`。不增加 `rc {}`/`arc {}` 语言语法。

`RC002`（Confirmed）：Rc/Arc handle 默认 Affine，复制所有权必须显式 clone。`Weak`
是普通库容器，循环由程序使用 Weak 打破，不提供 tracing cycle collector。Drop 必须
infallible；Rc 使用非原子计数且不跨线程共享，Arc 使用原子计数，并要求 payload
满足编译器推导的线程安全 sysmeta。

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
