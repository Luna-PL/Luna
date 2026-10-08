# Slot/Fragment 运行时注入计划

[English](slot_fragment_runtime_plan.md) | 简体中文

> 状态：已确认实施计划，2026-09-23
> 范围：类型安全、由宿主控制的运行时注入
> 当前验收视图（2026-09-28）：见 [v1 验收快照](slot_fragment_contract.zh-CN.md#v1-验收快照2026-09-28)。
> 原生宿主闭环已实现。仅用于测试的私有 JIT 现可执行受限的单 region 源码 Ref／apply，包括顺序和 outlined Slot 站点。公开源码入口／返回、container 发布及完整清理契约仍未完成；详细边界见下文。

## 模型

`slot` 是名义化、固定的注入点。声明 Slot 本身就是显式选择；最终表面不再提供
`runtime slot`，也没有可传递的 `RuntimeSlotRef`。`fragment` 是只针对一个确切
SlotId 的受限处理器。它可以通过 `resume` 获得 Slot 续体，但即使参数形状相同，也
不能与普通函数互换。

Slot/Fragment 专属源码关键字限定为四个：

```text
slot  fragment  apply  resume
```

`export` 与 `meta` 仍是全语言通用声明机制。`interceptor`、`context`、`many`、
`abort`、`default`、`runtime fragment` 和 `runtime slot` 不属于最终 Slot/Fragment
表面。首个 ABI 只支持 unit-result、single-shot；`resume;` 是受限语句，不是函数调用。

```luna
export meta plugin(name: string, priority: i32);
export slot pipeline(value: i32);

@plugin("trace", 100)
export fragment trace[prefix: string](value) for pipeline {
    print(prefix);
    resume;
}
```

静态 apply 仍可特化为零 Runtime 成本。最终设计中，绑定或导入 Fragment 会创建
`RuntimeFragmentRef<S>`；它拥有或借用显式环境，并固定所属 module generation。
`apply` 借用该引用，每次 Slot invocation 都创建新的 single-shot activation。

## 源码级 Ref／apply 的实施切片（2026-09-28）

方向已确认：让 Luna 源码作为宿主，接收、选择并局部应用运行时引用。先打通宿主提供
的已验证 Ref，不同时引入源码 loader、全局候选索引、handler 重入或新关键字。
以下完整示例仍是**目标语法，不能通过公开编译／打包路径执行**。源码 Ref 拼写、
名义和借用检查、canonical Apply region，以及受限的私有 JIT 执行路径已存在。
公开 Ref 入口／返回和 container 发布仍未实现。

```luna
export slot pipeline(value: i32);

// 默认 move-only 参数是共享借用，不转移拥有权。
runtime fn host_entry(selected: RuntimeFragmentRef<pipeline>) {
    apply selected {
        pipeline(42) { print("base"); }
    }
}

// 显式 affine 参数才是 owning take；拥有型返回不携带局部 context。
fn transfer(selected: affine RuntimeFragmentRef<pipeline>)
    -> affine RuntimeFragmentRef<pipeline> {
    return selected;
}
```

### 表面、身份与所有权

专属关键字仍只有 `slot fragment apply resume`；`RuntimeFragmentRef<S>` 是内建类型名，
`S` 解析为确切 Slot 声明。首批复用名字 operand 文法：`apply trace[实参] { ... }`
组合静态 Fragment；`apply selected { ... }` 对 Ref 值作局部覆盖。Ref 不再接收环境实参，
环境已在构造时固定，不在 apply 或 Slot invocation 中重绑。首批不支持任意表达式
operand，例如 `apply choose() { ... }`，也不增加第二套 apply 关键字。

TypeId 以目标 Slot 的稳定名义声明身份参数化，不以参数结构、用户字符串或 metadata
判断相等；sealed ContractId 单独作兼容检查，产物验证与宿主入口都必须匹配。不把最终
ContractId 反向塞入它所依赖的 TypeId，避免身份循环。两个同形 Slot 不互换，旧引用
不自动升级 contract；相同 ShapeId／handle 布局不授予转换权。候选数据不是 Ref，
源码不提供空引用或 raw pointer 强转构造。

局部拥有型 Ref 是 affine：可移动、丢弃与返回，不可隐式复制。现有未标注 move-only
函数参数是共享借用，显式 affine 才转移拥有权。Apply 只求值一次并在整个 region 内
共享借用；同一个 Ref 可以连续 apply，每次 Slot 调用都建立新的 single-shot activation。
Single-shot 限制续体，不限制 Ref 的总使用次数。借用期间移动／释放 owner、借用逃逸、
从借用参数返回拥有型 Ref 必须被拒绝。

### 宿主桥与 generation 寿命

首批通过现有 verified generation／factory 路径取得 native Ref，再显式封装为恰含
一个精确 Slot 与一个 Fragment 的冻结状态；任意 BindingSet 不能冒充源码 Ref。入口
检查 nominal Slot、sealed contract、carrier ABI 和拥有／借用方式，失败在进入源码
回调前报告。原生 Ref check/transfer/drop 函数及私有生成 wrapper 已存在；**公开源码**
导入／返回／drop 桥及 carrier ABI 尚未定稿，没有 C 可传入的源码 handle，也不改
descriptor v1 或 execute 签名。

源码 owner 独占封装句柄；内部可共享环境／generation pin，不暴露源码 Copy 能力。
Drop 走 Runtime 专用桥，不能用 Luna `free` 释放 C++ 对象或插件环境。最后一个内部
pin 释放时，环境先于模块卸载；borrowed native 环境仍须显式 environment lease。
异常／return／`?` 都要正确清理 region context 与拥有型 Ref。

源码 `.bind` 暂不作为首批构造入口：现有 context 只有 BindingSet，没有 MoonRuntime、
所属 generation 或 catalog 回指。声明地址、runtime metadata 和 TLS 不能证明可逃逸
引用的模块寿命。未来自行构造需要显式、验证过且固定 generation 的 offer／构造
capability；取得方式单独设计，不能冒充已实现的反射能力。

### 局部 apply 与显式 context

从有效显式 parent 派生局部 context，仅把确切 `S` 替换成 One(ref)，保留其他 Slot。
嵌套同 Slot 遮蔽而非自动追加；退出后使用 parent。不发布全局 BindingSet、不调用
safe point、不枚举候选、不自动选胜者；宿主源码决定传入谁、在哪里应用。

Direct-call context effect 固定点必须识别 runtime apply，MoonIR verifier 独立复算。
首批不暗中制造空根 context；宿主可显式提供初始化过的 None context。Handler body
仍不能取得／传递 context 或直接／传递动态 dispatch。Context-aware 间接调用、
Ref-bearing exported Slot／Fragment payload、non-Copy 环境、non-unit 与 multi-shot
仍不开放；普通 runtime 入口的专用 Ref handle 桥不是公开 Slot 资源载荷 ABI。

### 内部承载方式取舍

| 方式 | 优点 | 代价／结论 |
| --- | --- | --- |
| 每次 apply 消费 native Ref，重新构造下次引用 | 实现表面最小 | 破坏借用和重复 apply，重复 factory；拒绝 |
| 源码 Ref 直接 Copy／引用计数 | 多处安装直接 | 改变 affine 表面，扩大别名／并发承诺；不选 |
| affine 句柄封装冻结 singleton BindingSet，局部 context 共享内部 pin | 复用验证／cleanup，不重建环境；源码不可 Copy | region 入口复制 Slot 索引／链 pin；首批选用，私有表示可优化 |

当前派生成本随 base Slot／链规模增长，不是 O(1)、零分配或任意 Ref／环境线程安全
承诺；它不增加逐 Slot 调用的反射／factory。后续先测量 region 频率与 context 规模，
不能去掉验证或借用可能被回调销毁的宿主存储。

### 交付顺序与完成门

1. **已实现的 native 支撑**：`makeRuntimeFragmentBindingOverrideFromSnapshot` 接受
   初始化的 None 或仅含一个精确 Slot 的冻结有序链；`makeRuntimeFragmentExecutionContextOverride`
   从显式 parent 派生。输入不消费、失败不发布，与原 vector override 共用合并实现。
   Native 有序链不等于源码 Ref 可装多个 Fragment。
2. **部分实现的类型／产物**：内部名义 Ref／ownership／资源事实和内存中冻结／恢复
   已完成，见下文。源码夹具能解析带 `i32` 参数的 exported Slot，LLVM 也能生成局部
   Ref Drop／transfer。完整 compiler dropGlue 和 Ref container round-trip 仍待完成；
   旧 wire type 值不漂移，当前读写端明确拒绝内部 Ref。
3. **部分实现的宿主桥**：native 严格 singleton owning handle、精确目标检查、借用派生
   context 与 Runtime drop 已完成，见下文；一次性 unit 入口和直接 affine 返回
   wrapper 已验证私有 carrier 转换。公开源码导入、拥有／借用参数和返回 ABI、
   compiler dropGlue 与两包 carrier／contract 校验仍未接通。
4. **部分实现的源码 apply**：局部 Ref 精确 Slot 识别、环境实参拒绝、词法共享借用、
   canonical Apply 记录、context-effect 固定点和独立 CFG flow 计划已具备。一次性
   CodeGenerator 与仅测试用的 JIT 能执行一个顶层 region，以及其中借用同一本地 Ref
   或第二个借用 Ref、针对相同或不同精确 Slot 的一个内层 region。Jump 转换从有效父
   context 派生／释放每层 context；正常及 outlined return 按内到外释放 context 栈。
   顺序与 outlined Slot、
   重复 apply 和 generation pin 释放均已有验证。双 Slot JIT 夹具验证了内层 region
   保留外层 Slot，并在退出后恢复外层分派。outlined 回调中的内层 Apply 已有正常
   退出和嵌套 return 的 JIT 证据。非 Jump context 转换和 `?`／可恢复失败清理仍未
   证明。生产 module verifier、
   container encoder 和公开 CodeGenerator 仍拒绝 Ref 发布。
5. **端到端完成门**：真实两包 verified container 的 Ref 导入／源码应用、参数／返回
   transfer 与错误候选。只有这些通过才能将 `source-ref-apply` 标为 implemented；
   方向确认和 native 支撑不关闭源码缺口、历史 TBD 或发布批准。

本轮 native 回归检查重复派生、同槽替换／None、其他槽保留、错误 contract／多槽／
未初始化输入／输出别名拒绝，且 factory 不重复。寿命矩阵扩为 1536 组，包含 owned／
borrowed 环境、直接／快照派生 BindingSet／C++ context／C ABI、执行中释放或替换、
调用方记录销毁、重复 resume、完成／逃逸／无效控制／抛出失败，并检查 cleanup 顺序。
并发测试中两个 reader 从冻结 parent 与宿主选择快照派生独立 context，另两个走原路径；
这是不可变 native 环境的回归，不开放源码 Send／Sync。

严格构建的完整非 hardware 门禁 77／77 通过（208.49 秒）；最后扩展并发夹具后另行
重建并运行 Runtime 两项、设计状态与 inventory，4／4 通过。Windows ASan／UBSan
Runtime 两项 2／2，通过 WSL Arch Linux Clang 22.1.8 C++17 的直接 Runtime 与并发
ASan／UBSan 编译／执行。方向状态门额外拒绝回退为 scope-open，不允许提前宣称源码
完成。本轮没有新的匹配性能采样，不改旧证据身份锚点或释放／发布门禁。

### 内部 Ref 类型准备（2026-09-28）

本切片只增加编译器内部 `TypeKind::RuntimeFragmentRef` 与
`Type::makeRuntimeFragmentRef(slotType)`，没有注册新的源码 predefined type 或扩展
`apply` parser。它的唯一类型边是确切 nominal Slot；TypeId 编码目标声明身份，ShapeId
保留目标签名结构，不依赖 runtime generation 或最终 ContractId。

形成检查要求具体、unit／single-shot／Copy-only Slot，拒绝函数／匿名同形 Slot、
未解析类型参数／inference／Unknown、Ref-bearing 目标载荷和不一致的契约事实。
Ref 为 owned／affine／Unique／Executable／Lexical，需要 Drop 而非 Luna Deallocate；
它本身是 Plain value，不拥有或表示 single-shot continuation。未标注参数借用，显式
affine 参数拥有；array／record／Result／closure 的 recursive cleanup 按既有规则推导。

普通 shape equality 不授予 Ref 转换权。Explicit conversion 与 ABI compatibility 检查
在原有身份／结构外还比较 Ref 所在位置的 nominal Slot 约束，覆盖 pointer／borrow／
callable signature／capture 和 nominal aggregate。即使外层 nominal TypeId 相同、
shape 相同但内部 Ref 目标发生变化，也不能通过包装擦除目标约束。这不是替代未来
宿主入口的 sealed ContractId 检查。

内存中的 MoonIR 类型表通过既有 `innerTypeId`／`referencedTypeIds` 固定 Slot 图，
TypeMaterializer 可以独立恢复身份和资源事实；修改前端对象不改变冻结结果。64 位
内部布局暂以一个 opaque handle（8 字节／8 对齐）准备，这不是已发布的 C carrier ABI。
此类型切片尚未接通 Drop bridge／dropGlue，不能将“能冻结”当成“已 verified／可执行”；
后续 native 桥见下一节，源码 Drop 门禁仍关闭。

旧 enum 的 Slot=27、Fragment=28、Unknown=40 不变，内部 Ref 仅追加为 41。Wire decoder
仍以旧上限 fail closed，writer 也拒绝输出此 kind；Module verifier 明确拒绝 published
Ref，LLVM helper 不用未知类型的 i32 fallback 偷跑。不能通过伪造 Drop/sysmeta 或填一个
不存在的释放符号绕过这项边界；将来解除门禁须同时接通真实宿主／释放桥与跨包验证。

回归位于 `core_contracts_test.cpp` 和 `moonir_canonical_sealing_test.cpp`，覆盖同形异槽、
稳定身份与 contract shape 分离、七种包装、nominal aggregate 载荷变化、17 类伪造
Ref 事实、无效／不具体目标、递归图遍历、冻结恢复与前端修改隔离、writer／container／
当时 LLVM 拒绝（现已由下文的定向 carrier／Drop 调用预备替代），以及将 ordinal 41
注入旧 type section 后 decoder 不发布半成品状态。
`core-contracts-test` 现也在 sanitizer 配置中直接插桩自己的实现对象，不改变 installed
Runtime archive。源码签名、借用 region、导入／返回／drop 和动态 apply 仍未完成；
`source-ref-apply` 保持 implementation-open。

本轮最终严格构建通过完整 77／77 非 hardware 门禁（237.00 秒），Windows
ASan／UBSan 的 builtin types、core contracts、canonical MoonIR、container model 四项
4／4 通过（14.29 秒），已核对核心实现对象确实带插桩，而不是仅链接 sanitizer。
WSL Arch Linux Clang 22.1.8 C++17 直接编译／执行核心类型测试的 ASan／UBSan 也通过。
设计状态、文档 inventory 与 diff 检查通过；不新增匹配性能采样、发布批准或解锁旧 TBD。

### Native Ref handle／Runtime Drop 桥（2026-09-28）

`RuntimeFragmentRefHandle` 是不可 Copy、可 move 的唯一拥有句柄，只能由
`makeRuntimeFragmentRefHandle` 消费一个已验证的 native Ref 创建。入口同时核对确切
SlotId 与 ContractId；空 Ref、错误目标和已初始化输出拒绝，失败不消费输入。
所有分配先于最终 move，逐个分配失败注入证明异常回滚不丢失 Ref 或发布半成品。
内部封装冻结的单 Slot／单 Fragment BindingSet，不能从 None、多槽或有序链直接导入。

`opaque()` 只借用；`release()` 将唯一所有权转给 raw carrier。新增 C ABI
`luna_runtime_fragment_ref_check_v1` 校验 live handle 的目标（该检查无 heap 分配），
`luna_runtime_fragment_ref_drop_v1(void**)` 在 cleanup 前清空 carrier。空 carrier／
地址和同一已清空 carrier 的重入 Drop 安全；复制 owning pointer、stale／foreign pointer
和并发修改仍不合法。Magic 不是任意指针验证或全局句柄登记机制。

`makeRuntimeFragmentExecutionContextOverrideFromRef` 借用句柄、复用冻结派生路径；
重复使用不消费 Ref、不重跑 factory，同槽嵌套替换而非追加，其他槽和 parent 保持原样。
每个派生 context 独立固定环境与 generation，Ref 或 parent 释放不影响它；最后一个 pin
释放后沿既有 Ref cleanup 路径先清理环境再释放模块。C++ reset 和 move assignment 也
遵守先分离／先安装再回调规则，live owner 的 cleanup rebind 不会被回调返回覆盖。

这只是 additive native bridge，不发布源码／container carrier ABI，不更改 descriptor v1、
execute 参数、旧 wire ordinal 或现有 compiler guards。源码签名、import／参数／返回、
compiler dropGlue、wire round-trip、region borrow 和动态 apply 仍待完成；
`source-ref-apply` 保持 implementation-open。没有全局候选索引、TLS 或反射热路径。
本轮不新增匹配性能采样，不改旧 evidence／发布门禁。

严格完整构建后非 hardware 门禁 77／77 通过（202.19 秒）；最后补充模块 cleanup pin
断言后，重建／执行 native Runtime 两项及设计状态／inventory，4／4 通过。Windows
ASan／UBSan Runtime 两项 2／2 与 WSL Arch Linux Clang 22.1.8 C++17 直接编译／执行
Runtime 插桩测试通过。C ABI compile fixture 验证新增 check／drop 的 C 函数指针签名；
diff 检查通过。上述是本地验证，不等同于远端 CI 或稳定发布批准。

### 源码 Ref 类型拼写与约束边界（2026-09-28）

新增预定义类型构造器 `RuntimeFragmentRef<S>`，仍是普通 Identifier，**不是新增关键字**。
解析器沿用现有泛型类型语法；语义解析只在该构造器的参数位置将模块 Slot 声明解析为
确切名义 Slot，普通同形函数／结构体或错误参数个数拒绝。`S` 必须满足此前的 unit、
single-shot、Copy-only 和具体化要求；未开放从任意声明地址构造 Ref。

语义约束统一现在比较 Ref 的名义 Slot 身份及 ABI 结构，不允许默认结构统一吞掉同形异槽，
也不允许同一 Slot 身份下目标签名变化被视作兼容。推断遍历会进入 Ref 的目标边；
前端重建 AST 时保存已解析名义身份。默认参数是 shared borrow，显式 affine 才是拥有；
不存在源码 Ref Copy 能力或 Handler 可重入的隐式许可。

分析快照覆盖合法声明、错误类型参数及缺失／过多参数。另有真实源码→MoonIR 回归证明：
类型可解析并降低为内部表示，但 verifier 必须拒绝可执行发布。源码 import、拥有参数／
返回传递、compiler dropGlue、wire round-trip 和动态 `apply` 仍未实现；
`source-ref-apply` 保持 implementation-open，且旧 container decoder 上限不变。

本切片严格完整构建与非 hardware 回归 77／77 通过（257.87 秒）；Windows ASan／UBSan
的 builtin types、analysis snapshot、canonical MoonIR 三项 3／3 通过，WSL Arch Linux
Clang 22.1.8 C++17 直接编译／执行 builtin 类型与语义约束插桩测试通过。设计状态、文档
inventory 与 diff 检查通过；这是本地验证，不表示远端 CI 或稳定发布批准。

### Native 拥有句柄入口／返回转移（2026-09-28）

新增 C ABI `luna_runtime_fragment_ref_transfer_v1(void** source, SlotId,
ContractId, void** destination)`，为未来拥有型参数与返回值复用同一条原语。
它要求来源／目标是不同的 live carrier cell、来源非空、目标为空，并先用已有 Ref
检查核对确切 SlotId／ContractId；成功清空来源并将同一唯一句柄交给目标。
失败不更改两端，不重跑 factory、不复制环境或 generation pin，也不分配 heap。
默认 shared-borrow 参数仅做 `check`，不能误用 owning transfer 使借用方取得 Drop 权。

Native 回归覆盖空／别名／占用 carrier、错误 Slot／Contract、已消费来源、owned／borrowed
环境的宿主→参数→返回→宿主往返、重复校验与最终一次 Drop。C 头文件编译检查新增
函数指针签名；Windows 与 WSL Linux 插桩执行通过。此原语尚未接到生成函数 ABI、
异常／return／`?` 路径或 compiler dropGlue，不能据此解除 MoonIR verifier／container
门禁，也不构成新的公开源码关键字或插件自动选取策略。

严格完整构建与非 hardware 门禁 77／77 通过（223.99 秒）。最终将 carrier 写入顺序
明确为“来源清空→目标安装”后，受影响目标重建并复测 canonical MoonIR、Runtime ABI、
Runtime Fragment／并发、设计状态、inventory 六项 6／6；Windows ASan／UBSan Runtime
两项 2／2 和 WSL Arch Linux Clang 22.1.8 C++17 直接 Runtime 插桩测试通过。
上述为本地验证，不表示远端 CI 或稳定发布批准；不改变旧性能证据。

### Compiler Ref Drop 调用预备（2026-09-28）

LLVM 内部将 Ref 表示为 opaque pointer，不再落入未知类型的 i32 fallback。局部及
canonical value cleanup 对直接 Ref 的 `Drop` 使用原始可写 carrier cell 调用
`luna_runtime_fragment_ref_drop_v1(void**)`；callback 同样直接使用收到的 cell。
聚合载荷若只有提取后的 SSA 值而无原始字段 cell，则显式报错，绝不复制句柄或调用
`rt_dealloc` 释放 native handle。JIT 注册了相同 runtime 符号。测试检查生成的 LLVM 调用目标、
cell 参数和模块有效性；native ABI 测试负责清空顺序与一次释放的语义。

这只是编译器清理调用预备，不是完整的 compiler dropGlue：聚合清理仍需
原始字段 cell 的清空和部分初始化／move 清理；源码函数入口、拥有型参数／返回
转移、borrow region、动态 `apply`、跨包 wire round-trip 均未接通。MoonIR
verifier 与 container gate 保持关闭，`source-ref-apply` 仍为 implementation-open。

本地严格构建与非 hardware 回归 77／77 通过（220.43 秒）；Windows ASan／UBSan
canonical MoonIR 1／1 通过。设计状态、file guide inventory 与差异检查通过。
这不表示远端 CI 或稳定发布批准。

### 内部 Ref 局部转移预备（2026-09-28）

内部 codegen 预备路径把直接局部 `move ref` 按“读取原 carrier → 清空原 cell →
交出值”的顺序降低；直接 `return ref` 对拥有型参数／局部也使用同一操作，
使后续退出清理回调看不到仍持有的来源。字段／索引投影的 Ref move 和 return
暂 fail closed，须先有原位投影 cell 和部分初始化／move 证明。LLVM 定向回归检查
清空指令紧随读取，且目标 cell 的 Drop 仍走专属 Runtime 符号。

这不是宿主到源码的 ABI：当前生成函数仍只接收裸 pointer 参数，尚未在入口核对
SlotId／sealed ContractId 或消费宿主 carrier cell；借用与拥有参数的跨边界协议、
返回转交宿主、聚合清理、wire round-trip 都未完成。MoonIR／container 门禁不变，
`source-ref-apply` 保持 implementation-open。

本切片严格构建及非 hardware 回归 77／77 通过（204.15 秒）；Windows
ASan／UBSan canonical MoonIR 1／1 通过。设计状态、inventory 和差异检查通过。
这是本地验证，不代表远端 CI 或稳定发布批准。

### 冻结 Ref 目标解析（2026-09-28）

新增 `Module::resolveRuntimeFragmentRefTarget(TypeRef)`，仅在类型表已封存时从
`RuntimeFragmentRef<S>` 的冻结 inner type 找到同一名义 Slot 声明，返回精确
SymbolId／ContractId。它重新推导 Slot 和 Ref 的 TypeId／canonical type，核对
声明类型、symbol、canonical contract 与 sysmeta identity；同形异槽替换、
错误声明类型、伪造 contract 或未封存表均拒绝。回归使用真实源码 lowering 的
两座同形 Slot，并在恢复原表后继续确认 verifier 仍禁止 Ref 发布。

这只解决未来入口需要的目标事实，不验证任何 live native handle，也不改变生成
函数的裸 pointer ABI。借用参数需要在宿主边界只读 `check`；拥有参数必须通过
独立 carrier cell 进行校验后转移，不能先复制裸 pointer 再称为 owning。
入口错误的报告方式、跨包证据、返回交付和 wire round-trip 尚未完成；
`source-ref-apply` 与 MoonIR／container 门禁保持原状。

本地严格构建及非 hardware 回归 77／77 通过（209.57 秒），Windows
ASan／UBSan canonical MoonIR 1／1 通过；设计状态、file guide inventory 和
差异检查通过。这不代表远端 CI 或稳定发布批准。

### Ref 宿主入口 LLVM 调用预备与二次门禁（2026-09-28）

编译器内部现有两条互不混用的 IR 调用构造：共享借用只对 live handle 调用
`luna_runtime_fragment_ref_check_v1`；拥有型入口用来源／目标两个 cell 调用
`luna_runtime_fragment_ref_transfer_v1`。两者接收完整的 `DeclarationRef`；定向
回归将冻结 Ref 目标解析所得的 SlotId／ContractId 传入，并核对身份常量、调用目标
与 carrier 参数位置。返回状态留给未来入口 wrapper 处理；JIT 已注册这两个 Runtime 符号。

`CodeGenerator::generate` 现在也在 Ref 类型上 fail closed：即使有人绕过 MoonIR
verifier 直接调用后端，也不能把尚未包装的裸 pointer 函数发布成宿主入口。
这里仍未生成或执行入口 wrapper，未定义错误状态到源码函数返回的映射，也没有
跨包 import／拥有返回协议。原 MoonIR／container 门禁和 `source-ref-apply`
implementation-open 状态不变。

严格构建和非 hardware 回归 77／77 通过（195.47 秒），Windows ASan／UBSan
canonical MoonIR 1／1 通过，WSL Arch Linux Clang 22.1.8 对改动涉及的后端文件
完成语法编译检查。设计状态、file guide inventory 和差异检查通过；这是本地验证，
不代表远端 CI 或稳定发布批准。

### Ref 宿主入口状态分支预备（2026-09-28）

编译器内部新增借用和拥有两种入口状态门控，要求宿主 wrapper 返回 `i32` 状态。
借用只检查句柄，不取得 Drop 权；拥有路径从调用方 carrier 转入独立且初始为空的
被调用方 carrier。仅状态为零才进入函数体；失败时在执行用户代码前原样返回 Runtime
状态。LLVM 定向回归核对两条分支、失败返回值、冻结 Slot／Contract 身份、拥有
carrier 参数位置，并拒绝无状态返回值的 wrapper。拥有型测试的成功路径清理其
被调用方 carrier。

这仍是内部 LLVM 构件，不是已生成或执行的源码宿主 wrapper；源码通用返回／错误
ABI、跨 callback 的借用期限、拥有返回交付、聚合 dropGlue 与跨包 import 均未定义完成。
直接后端、MoonIR verifier 和 container codec 仍禁止源码 Ref 发布；
`source-ref-apply` 继续保持 implementation-open。

严格构建和非 hardware 回归 77／77 通过（205.65 秒），Windows ASan／UBSan
canonical MoonIR 1／1 通过；WSL Arch Linux Clang 22.1.8 对改动的后端文件
完成语法编译检查。设计状态、file guide inventory 与差异检查通过；这些是本地
验证，不代表远端 CI 或稳定发布批准。

### 私有单 Ref 宿主 wrapper 原型（2026-09-28）

内部 LLVM emitter 现在可围绕 `void` 函数体生成完整的状态返回 wrapper：函数体
只有一个 Ref 参数，可选一个显式前置 Fragment execution context。借用入口检查
live handle；拥有入口先把宿主 carrier 转入初始为空的私有 cell，再在调用函数体前
读取并清空该 cell，把唯一句柄交给函数体。校验／转移失败原样返回 Runtime 状态，
不调用函数体，也不改变调用方拥有型 carrier。wrapper 仍为 internal linkage，
不进入 Runtime descriptor。IR 回归核对函数体调用、context 透传、owner cell
清空、失败分支、冻结目标以及无效目标／context／名字的拒绝。

此原型本身不能证明被调用方 CFG 会按借用或拥有关系处理 Ref 参数并执行相应
cleanup。发布前仍须完成该证明、真实源码声明接线、返回 ABI 与跨包执行。
MoonIR／后端／container 门禁保持关闭，`source-ref-apply` 仍为
implementation-open。

严格构建和非 hardware 回归 77／77 通过（201.50 秒），Windows ASan／UBSan
canonical MoonIR 1／1 通过；WSL Arch Linux Clang 22.1.8 对改动的后端文件
完成语法编译检查。设计状态、file guide inventory 与差异检查通过；这些是本地
验证，不代表远端 CI 或稳定发布批准。

### 冻结 Ref 入口所有权配对（2026-09-28）

私有单 Ref／unit wrapper 不再接收调用方指定的借用／拥有模式、目标或 context
标志，而是从模块中真实存在的 Function 声明推导。构造要求封存的名义 Ref 目标、
unit 返回、完整的冻结 Function 类型及声明契约、匹配的唯一 canonical 参数和
独立验证的 sealed CFG。共享借用参数不得有 cleanup；affine owner 必须恰有一条
根作用域直接 Drop cleanup。Fragment context effect 从模块调用图独立复算，
再确定入口 ABI 形状；LLVM 函数体符号及指针参数数目也必须与源码声明一致。
对抗回归篡改参数关系、cleanup、函数类型 canonical、Ref 资源事实和 context effect，均拒绝
生成 wrapper；不相符的 LLVM 函数体名字亦被拒绝。

这只证明内部所有权模式的选择，不证明任意传入的 LLVM 函数体实现了该 CFG。
下文的私有生成体配对针对两种 unit 入口形状补上后一缺口，但不发布 descriptor；
MoonIR／后端／container 的 Ref 门禁保持关闭。源码 import、返回、聚合清理和
动态 `apply` 仍未完成。

最终代码的严格构建和非 hardware 回归 77／77 通过（193.67 秒），Windows
ASan／UBSan canonical MoonIR 1／1 通过；WSL Arch Linux Clang 22.1.8
对改动的后端文件完成语法编译检查。设计状态、file guide inventory 与差异
检查通过；这些是本地验证，不代表远端 CI 或稳定发布批准。

### 私有 Ref 入口与真实生成体配对（2026-09-28）

直接绕过 MoonIR 总体验证而调用 CodeGenerator 时，单 Ref 参数／unit 返回的源码
入口现在由真实 canonical CFG CodeGenerator 降低到一个独立、短命的 LLVM module。
降低之前先独立验证 sealed CFG；冻结的源码签名及 CFG 决定私有 wrapper 的所有权
模式和名义目标，wrapper 随后调用该次实际生成的函数体。验证要求两者均为 internal
linkage、wrapper 恰调用该函数体一次、affine 拥有型参数的生成体含 Ref Drop、
共享借用参数不含 Ref Drop，且完整 LLVM module 通过 IR 验证。canonical 回归证明
两种源码入口均通过私有验证，篡改所有权关系则失败。

即使验证成功，临时 module 和 wrapper 也会立即丢弃；发布用 CodeGenerator 仍返回
阻断诊断，JIT／AOT 不能从这一步取得生成体。当前范围故意只覆盖一个 Ref 参数和
unit 返回；需要其他函数体或额外 lowering 支持的入口可以在私有验证中失败，
不会打开发布门禁。这还不是源码宿主 ABI、descriptor、import、返回值传输、wire
往返、动态 `apply` 或跨包执行。`source-ref-apply` 仍为 implementation-open。

严格 Windows 构建及本地 77 项测试全部通过，Windows ASan／UBSan canonical 测试
亦通过；WSL Arch Linux Clang 22.1.8 对改动的后端文件完成语法编译。
设计状态、file guide 和差异检查通过；这不是远端 CI 或发布批准。

### 私有 affine Ref 返回函数体证明（2026-09-28）

真实源码 `fn transfer(selected: affine RuntimeFragmentRef<S>) -> affine
RuntimeFragmentRef<S> { return selected; }` 现在进入第二个一次性 CodeGenerator
证明。它要求参数与结果具有完全相同的冻结名义 Ref 类型、拥有／affine 契约、无隐藏
runtime context、独立验证的 sealed CFG，以及直接返回唯一参数 local。生成的内部
LLVM 函数体必须从参数 carrier 取出句柄、在返回前清空该 carrier，并通过完整 LLVM
module 验证。篡改结果的所有权用法会使证明失败。前一轮 unit 返回的函数体／wrapper
证明与本项分开。

这仅证明被调用方狭义的所有权移动，不是宿主返回 ABI。下一私有切片增加携带状态码
的输出 carrier 与失败清理；跨包 import、descriptor 和 callable 发布仍不存在。两个
证明 module 均立即销毁，直接 codegen 仍拒绝所有源码 Ref 发布。
`source-ref-apply` 保持 implementation-open。

### 私有 affine Ref 返回 carrier wrapper（2026-09-28）

直接 affine 参数到返回值的证明，现在把真实生成的函数体与内部
`(输入 cell, 空输出 cell) -> 状态码` wrapper 配对。消耗输入前先拒绝空地址、
输入输出同址以及非空输出；随后按冻结 Slot／Contract 调用 native transfer，
把输入移至私有 cell、取出 owner 交给函数体，再用同一目标把返回 owner 移至
宿主输出。返回转移失败时，wrapper Drop 私有返回 cell 中仍拥有的句柄。
源码 CFG 限定为无回调的直接参数返回，因此不会在输出预检与第二次转移之间
修改调用方输出；宿主并发修改 carrier 依旧被禁止。

进入函数体前的失败保持输入和输出不变；入口转移成功后输入即已消耗。
如果之后返回转移意外失败，会清理返回 owner，而不承诺回滚输入。私有证明
要求一次生成函数体调用、两次 native transfer、一次失败 Drop、internal linkage
及有效 LLVM IR。结构回归核对输出空位、两次冻结身份、函数体到输出的 carrier
流动和无效函数体／契约拒绝。这不是已发布的返回 ABI，也不证明任意传入的 LLVM
函数体实现源码 CFG。临时 module 随即丢弃；MoonIR／后端／container 的源码 Ref
发布门禁仍保持关闭。

### 源码 Ref apply 的识别与借用门禁（2026-09-28）

以下直到 context 安放计划的分节记录各切片落地时的状态。当前私有执行边界和发布缺口
以上文交付顺序及下文 2026-09-29 的源码 body 证明为准。

既有 `apply name { ... }` 拼写现可区分局部 `RuntimeFragmentRef<S>` 与静态
Fragment：解析精确导出的 Slot、拒绝环境实参，并在词法 body 内共享借用 owner。
同一 Ref 可以重复 apply，body 内移动 owner 会被拒绝。前端分析中，动态绑定会遮蔽
同槽的外层静态绑定。在这一时间点**尚无可执行的源码 apply**：canonical region
与 effect 验证还未生成函数体，module 发布会报告 context-override 尚未实现。
后续私有 JIT 已能执行受限 body；公开 operand／context 执行、return／错误清理
及跨包测试仍是完成门。

严格完整构建通过。并行非硬件回归 76／77 通过；`luna.repl-smoke` 的进程树清理在
并行负载下超时，单独重跑通过。最终聚焦的分析、canonical、REPL、文档和清单
门禁 5／5 通过。这不是源码 Ref 端到端验收，也不是新的性能观察。

### Ref 参数的 context effect 与嵌套借用验证（2026-09-28）

既有的密封 CFG effect 固定点，现经回归验证可标记直接调用 exported Slot 的 Ref 参数
函数，以及传递到它的 Ref 参数调用者。私有 unit 入口在生成 LLVM 函数体之前，会独立
重算 effect 并拒绝伪造的传递标记。前端回归还证明嵌套共享 `apply` 借用、拥有型返回前
释放借用，以及在借用作用域内带走 owner 会被拒绝。当时这只验证 Ref apply 周边
机制；私有 region override 和可执行 lowering 后续才落地。公开 ABI 与 wire 格式
未改变。
严格完整构建与全部 77 项非硬件门禁通过（156.55 秒）。

### 内部 canonical Ref-apply region（2026-09-28）

源码 Ref apply 现可先进入结构化 MoonIR，再落为内存中的 canonical `Apply` region。
独立的 `RuntimeRefApplyRecord` 将该 region 与局部 Ref owner、精确导出的 Slot
关联。CFG verifier 检查 region 类型与唯一性、词法作用域、局部 Ref 类型，以及
Slot／Contract 的名义匹配。动态绑定遮蔽同槽外层静态 Fragment，因此 body 中的
Slot 站点变为运行时分派，并参与既有 context-effect 固定点。在这一时间点它只是
证明边界。密封 module verifier、container encoder 和公开 CodeGenerator 仍拒绝
发布；后续一次性私有 CodeGenerator 已接受受限 region。冻结的 Moon Container
0.3 布局与公开 ABI 均未改变。完整退出／错误清理、wire／公开 ABI 设计及端到端
验收仍未完成。

### 编译器私有的 Ref context 桥接（2026-09-28）

Native runtime 现提供未发布、仅供编译器使用的桥接：从借用的存活 Ref handle 和
显式父 context 派生独立拥有的 context 指针。它核对精确 Slot，保留父 context 的
其他绑定，复用不可变快照 override，且仅在全部分配成功后写入输出。拥有型 cell
的 Drop 先清空 cell，再释放快照。嵌套派生和 Ref owner Drop 后仍可使用的行为已有
回归验证。在这一时间点 LLVM 仍拒绝 Ref-apply CFG；后续私有证明已生成受限源码
body 的入口／出口代码。公开 CodeGenerator 仍拒绝 Ref 发布，完整 return／错误
清理、wire 格式和公开 ABI 仍未完成。此桥接不属于稳定 runtime Fragment ABI。

### Canonical Ref-apply context 安放计划（2026-09-28）

新增不进入 wire 的编译器分析，沿每个 CFG block 的 Ref-apply region 祖先链计算
context 栈。每条后继边记录按内到外释放、按外到内构造的 context；`return` 与
`unreachable` 终止路径单独记录。进入 Ref-apply region 只能经过其确切入口 block。
独立 CFG verifier 执行此分析，因此伪造边不能绕过 context 构造。回归覆盖正常
进入／退出、绕过入口，以及嵌套提前返回时内层先释放的义务。在这一时间点它仍只是
安放**计划**，尚未生成清理指令。后续私有证明已生成一个顶层 region，并验证
outlined Slot 的 context 继承与 return 清理。后续私有证明核对分派失败时先 Drop
再 trap；可恢复失败清理仍开放。不改
container 或公开 ABI。

### 私有 LLVM Ref-apply context 转换证明（2026-09-29）

可复用的 LLVM 辅助函数现可携带精确 Slot／Contract 常量，生成编译器私有的
Ref-to-context 派生和拥有型 cell Drop 调用。一次性 internal LLVM 函数使用
真实的嵌套 Ref-apply CFG 计划，验证先外后内的进入顺序、使用 context 前的
状态检查、内层派生失败时释放外层，以及提前返回时先内后外 Drop。无效辅助函数
实参被拒绝，生成的 module 通过 LLVM 校验。该函数只模拟 context 转换，
**并未**生成源码 body 或被 outline 的 RuntimeSlot continuation。源码 module、
container encoder 和公开 CodeGenerator 门禁仍关闭；稳定 ABI 与 wire 格式不变。
私有桥接符号已登记到显式 JIT runtime 符号表，但尚无可发布的源码 module 引用它们。

### 私有一至两层 region 源码 Ref-apply 函数体（更新于 2026-10-04）

一次性 CodeGenerator 现可生成**真实 canonical 源码 body** 的受限情形：一个顶层
Ref apply，以及可选的一个内层 Apply，两层借用同一本地 Ref 或各自借用一个 Ref，
针对相同或不同的精确 Slot。每层 region 有独立拥有型 context cell。私有证明分别核对
每次派生对应的参数 carrier 与精确 Slot／Contract 常量。Jump 入口从当前父 context
派生；Jump 出口与 return 在借用 Ref owner 释放前，按内到外执行局部 cleanup 和 Drop。
私有证明将每条 CFG 进入／退出义务对应到 LLVM derive／Drop，核对 Slot 分派使用
最内层有效 cell，并递归检查生成的 continuation callback。每个 frame 在分派前
捕获完整 owner 栈；outlined return 在逃逸前释放该栈，不能留下未验证的生成分派。
顺序与嵌套 outlined Slot 站点均受支持；证明体至少有一个 RuntimeSlot 站点。
内层 Apply 可经 Jump 边在 outlined Slot 回调内进入与退出；回调捕获其 Ref carrier，
派生回调局部 owner cell，并把当前 owner 栈传给更深的 Slot 回调。私有证明核对
回调副本的每次派生、分派、Jump 退出和 Return，包括失败分支 Drop。非 Jump context
转换和可执行的 unreachable 终止仍被私有代码生成拒绝。唯一允许的 unreachable
终止是穷尽两个标签、入边不改变 context 的 Result Switch 默认分支。
生产入口证明仍仅支持 unit 返回；仅测试用的私有入口也接受受限 Ref-apply body 的
`Result<i32, i32>` 返回。
派生失败保持输出 cell 为空，释放有效父 cell 后 trap。Slot 分派返回负状态时，
源码 body 与 outlined 回调都会先按逆序 Drop 全部有效 cell，再 trap。私有证明
把每个失败分支连回对应派生／分派，并核对 owner 栈及顺序；续体 return 已清空
cell 后重复 Drop 也安全。可恢复的派生／分派失败尚无源码级返回协议。module verifier、
container encoder 和公开 CodeGenerator 仍拒绝
带 Ref 的发布，因此这并非端到端可执行源码支持，也不改变公开 ABI。

canonical 回归目标另有仅在测试编译时启用的私有 JIT 入口。它把已经验证的源码
body 放进内部测试 wrapper，以真实借用 Ref 和空父 context 执行正常退出、Slot
分派后的提前 return、分派前的提前 return、两个顺序 Slot 站点，以及
一层、两层 outlined continuation 内的嵌套 Slot 站点与其中的 return，以及两个
嵌套 Apply region 的正常出口和内层 outlined return。双 Ref 夹具用同 Slot 的不同
descriptor 核对外／内／外分派来源与参数。双 Slot 夹具核对外层 `checkpoint`、
内层 `shadow`，以及内层之中和退出后的外层 `checkpoint`，包括精确 Slot／Contract／
布局和参数 payload；第二个双 Slot 夹具从内层 outlined 续体 return，核对逆序 owner
清理路径。
回调局部 Apply 夹具核对正常退出时的 Slot 参数顺序，以及内层 outlined return 在
释放有效 owner 栈后逃逸。generation lease 在调用期间保持固定，宿主随后释放一或两把
Ref handle 后即失效，证明这些路径没有留下派生 context 的 pin。每个 body 都使用
同一把或两把借用 Ref handle 连续调用两次，以核对重复 apply 不消耗宿主 handle。
此入口还读取每次 activation 的打包参数，确认两个顺序站点在两次
调用中均按 `1`、`2` 分派。生产编译器目标不含此入口，也不生成公开 descriptor
或 container code。

### Ref apply 内的源码 `?`：私有执行验证（2026-10-05）

新增的真实源码夹具在两层嵌套 Ref Apply 内使用 `?`。生成的 Result Switch 在 Ok／Err
边上仍保留两层有效 region；Err 分支是 Return 终止点，flow plan 要求先释放内层
context，再释放外层 context。普通 CFG verifier 接受此路径。私有 JIT 入口现生成
真实的 Result 返回 body，并通过仅用于测试的标量值观察标签和 `i32` payload。
两个源码函数分别以本地 `Ok(9)` 和 `Err(7)` 为输入：Ok 分派 `checkpoint(9)`，
返回 `Ok(0)`；Err 在分派前返回 `Err(7)`。两条路径都使用真实借用 Ref 各执行两次。
私有证明核对派生 context 与 Drop，宿主释放 handle 后 generation pin 失效。
另两个私有 JIT 夹具分别在 outlined Slot 续体内返回 `Err(7)`，以及在先前 Slot 分派
正常完成后通过 `?` 传播错误。前者验证 Result 在回调 frame 中的存储与逃逸，后者
验证已完成分派之后的 Err 仍按先内后外清理。两者都使用真实借用 Ref，并核对分派
次数及返回 payload。
另一个 Err 夹具在 `?` 之前分配带源码 `Drop` 方法的 apply 局部 struct。仅测试用的
JIT 导入其精确冻结的 Drop glue 和外部探针；两次调用各以预期 payload 执行一次 Drop。
私有 LLVM 证明核对该局部存储，并验证 Drop、`rt_dealloc`、context Drop 的顺序；
ASAN canonical 目标通过。
新增 Err 夹具将 `Result<i32, SourceError>` 的仿射源码错误经
`From<SourceError> for i32` 转换，函数返回仍是 `Result<i32, i32>`。仅测试用的
JIT 只导入 canonical Err Return 精确引用的冻结 `From` 方法；私有 LLVM 证明要求
转换调用恰好一次且先于两层 context Drop。两次执行都在没有 Slot 分派时返回
`Err(47)`，宿主释放 handle 后借用 Ref 的 pin 失效。仿射源码错误带有源码 `Drop`，
由转换方法消费；仅测试用的 JIT 顺着该方法的冻结 cleanup 表导入精确的 Drop glue，
探针证实每次调用各 Drop 一次。返回 payload 仍是标量。
另一个 `From<SourceNestedError> for i32` 夹具在相同的两层 Apply／`?` 路径
消费带一个拥有型 `SourceError` 字段的仿射外层错误。转换有两条返回标量的分支：
正值分支先将整个仿射错误 move 到分支局部 owner，再返回其 marker；负值分支
从原参数返回。每条分支各执行两次，均不分派 Slot 且返回 `Err(43)`。
正值分支先 Drop 外层 `43`、
再 Drop 内层 `47`；负值分支先 Drop 外层 `-43`、再 Drop 内层 `47`。Windows
Clang64／LLVM 20 与 WSL Arch Linux／LLVM 22 的聚焦 canonical ASAN 测试均通过。
这证明有界双节点转换的一次条件式整对象局部 move 和两条出口；部分字段转移、
向宿主转移所有权及更宽的转换函数体仍未证明。
一个含两个拥有型字段的源码探针现于 `move pair.first` 的源码所有权检查阶段
明确拒绝，并指出位置及整对象 move 的替代做法。此前源码检查器记录了被移动字段，
return 清理却仍指向根 owner，随后 MoonIR sealing 因缺少字段投影清理记录而拒绝。
普通 struct 目前仍只有根对象清理记录。回归夹具固定了前移后的拒绝行为。
扩展 `From` 字段转移前，源码 lowering 须生成互不重叠的字段清理记录与底层
allocation 释放，沿每条终止分支传递精确的剩余义务。手工构造的封闭 CFG 现验证了
第一步 canonical 表和数据流：含两个 String 字段的拥有型参数可以返回其中一个
move 后的字段，并先清理剩余字段再释放 allocation；没有字段转移时则先清理两个
字段再释放。verifier 拒绝遗漏或重复字段、重叠的整对象 Drop、颠倒的字段顺序、
仅释放 allocation 而泄漏字段，以及错误的 return 清理列表；字段转移时根
allocation 清理记录保持有效。这仅是 verifier 证据：源码 lowering 仍生成根清理
记录，源码字段 move 仍被拒绝，分拆路径尚无 JIT 中 Drop 和释放的执行证明。
另一源码夹具在相同的两层 Ref Apply 和 `?` 结构中返回
`Result<i32, ReturnedResource>`。封闭 CFG 验证拥有型 payload 的 move 与先内后外的
context 退出。更窄的仅测试用 JIT wrapper 在源码 body 返回后观察资源 Err，调用精确
冻结的 Drop 方法，再调用 `rt_dealloc`。两次调用都在没有 Slot 分派时得到 marker
`59`，Drop 探针每次触发一次。wrapper 现还覆盖完成 Slot 分派后的资源 Ok
（marker `67`），以及 marker 位于第二字段的双字段资源 Err（marker `61`）；后者
按冻结的 product 偏移读取字段。每个 owner 每次调用各 Drop 一次。同一 Result 形状的
标量 Ok／Err 分支分别返回 `14`／`13`，不调用资源 Drop。源码 Drop 方法在探针后把
`marker` 清零，因此返回的 marker 还验证 wrapper 在最终清理前
完成观察。该阶段的仅测试用 wrapper 最多接收两个顶层字段：一个 `i32` marker，另一个为
`i32` 或下述有界内嵌资源。它在 JIT module 内消费 owner，不向宿主交付资源指针或所有权
carrier。这些夹具的 canonical ASAN 目标通过，但仍受下述独立的间歇性 COFF loader
故障限制。
另两个 Err 夹具分别返回两层和三层 struct 所有权链。wrapper 在读取外层 marker 后复用
编译器现有的递归拥有型 payload 清理；窄形状门禁最多准入三个各有冻结源码 Drop 与
`i32` marker 的 struct。私有 LLVM 检查要求 Drop 从外到内、deallocation 从内到外。
两次 JIT 调用分别观察到 `71, 73, 71, 73` 和 `79, 81, 83, 79, 81, 83`。
ASAN canonical 目标通过；在该检查点，更长或分叉的所有权图仍不在证明范围内。
后续私有夹具将形状精确扩至三个拥有型 struct 节点的有界分叉：外层 marker 加上
两个各自带 marker 和冻结 Drop 的独立拥有字段。形状门禁统计整棵树的节点；
已封闭的四节点分叉负例在 JIT 物化前被拒绝。`?` Err 返回的两次 JIT 调用均按字段
顺序观察到 `89, 91, 93`；私有 LLVM
检查还要求外层 Drop、左字段 Drop／释放、右字段 Drop／释放，最后释放外层。
宿主交接实验在注入的 body 后失败清理与成功交接后的恰好一次宿主 Drop 中重复
验证该顺序。Windows CLANG64 和 WSL Arch Linux 的普通及 ASAN canonical 聚焦
测试均通过。
此前 Windows Clang64 ASAN（LLVM 20.1.8）canonical 测试共运行五次，其中一次在 LLVM COFF
JIT 装载时以 `IMAGE_REL_AMD64_ADDR32NB relocation requires an ordered section layout`
失败；紧接着的重试及其后三次重复运行均通过。增加资源返回 CFG 夹具后又有一次通过。
失败发生在较早的 sealing-test 阶段，早于注册的 Ref 夹具；这是 JIT loader 失败，
不是 ASAN 内存诊断。Windows LLJIT 默认 object layer 使用 RuntimeDyld，尚未锁定具体
触发的 JIT 调用。canonical 测试现对各 sealing 子测试和最早三个 JIT 调用标记阶段；
增加标记后连续 12 次 ASAN 重跑均通过，未再捕获重定位失败。定位这一 COFF section
布局间歇故障前，ASAN 结果须附带此限制。
2026-10-08 新增嵌套转换夹具前，又完成 20 次 canonical ASAN 重跑，仍未复现该错误。
一次只作用于测试目标的独立实验把 Windows object layer 改为 LLVM 20.1.8 的
JITLink。ASAN canonical 的一次运行在更早的 compiled-host 夹具链接时失败：
`.pdata` 到 `.text` 超出 `Pointer32` fixup 范围。该实验改动已撤回；默认
RuntimeDyld 构建随后通过同一 canonical 测试。因此直接切换此 JITLink 配置
不能作为已验证的间歇性 RuntimeDyld 故障修复。
私有 JIT 另有一组仅测试用的宿主所有权交接入口。状态入口先检查 tag／owner 输出
地址非空、互不重叠且 owner cell 为空，再调用源码 body。标量分支只写 tag；资源
分支先写 tag，最后写 owner 指针作为所有权提交点。独立 JIT Drop 入口先清空 cell，
再按冻结的递归 Drop 与 deallocation 顺序清理。私有 LLVM 检查要求交接入口只调用
一次 body，且只有注入失败分支可额外调用清理函数，并核对两条 Drop 调用链。
三层 struct Err 夹具证实成功交接
的 owner 在宿主显式 Drop 前不被清理，之后每个 owner 恰好观察 `79, 81, 83` 一次；
重复 Drop 失败。资源 Ok 分支也每次交接并 Drop 一个 owner；对应的标量 Ok 与 Err
夹具保持 owner 为空且不执行资源 Drop。已占用、重叠或
空输出地址在 body 分派前失败且不改动输出。注入的失败在源码 body 返回非空资源后
触发，两个输出保持原值，JIT 在返回状态 `3` 前执行同样的精确清理。第二次成功
交接后，宿主延迟 Drop，先释放借用 Ref handle，并确认 generation pin 失效而资源
Drop 次数不变。随后释放原始 JIT 句柄，由另一份共享 LLJIT lease 保持 Drop 入口
可执行；宿主恰好执行一次 Drop 后该 lease 才结束。裸 owner 指针本身不保留 JIT
代码；公开 carrier 必须同时持有 owner 与代码 lease，直到清理完成。这仍只是私有
实验，没有公开 carrier 或符号契约；尚未确定生产失败状态，也未验证 owner 未释放
时安全关闭 JIT 的行为。
该观测值不是公开返回 carrier 或稳定 ABI。超过三个拥有型 struct 节点的图、
更宽的资源图、
带条件式部分字段转移、向宿主转移所有权或更宽所有权图的转换函数体、
outlined Slot body 中的 `?`，
以及可恢复的派生／分派失败，均不在此执行证明内；前端继续在
outlined Slot 边界拒绝 `?`。

当前源码 builder 通过 Jump 边进入 Ref Apply，并在正常结束时通过 Jump 边退出；
这些结构化路径不生成源码级的非 Jump context 转换。flow planner 会计算所有边类型
的义务，但私有 codegen 明确拒绝非 Jump 转换。保留此门禁，直到未来的源码 lowering
产生可达的相应边，并有可执行的清理证明。

另一仅测试用的状态入口现把只读 parent context 预检、精确的 native 借用 Ref
检查与一个已封闭、返回 unit 的
源码 Apply body 组合；SlotId／ContractId 来自该 body 参数的冻结目标。真实宿主
构造的 Ref handle 连续两次各成功分派一次；空 handle 与指向另一精确 Slot 的有效
Ref 分别返回不同的私有无效句柄／目标状态，均不进入 body。空 parent context
也在 Ref 检查和 body 前失败。私有 LLVM 检查要求一次 context 检查、一次 Ref
检查及一次 body 调用，并核对精确的状态分支、目标 block 和返回常量。
compiler-private context 检查接受 Runtime 构造的有效 parent，
拒绝空值且不分配。两个 handle 在释放前都保留 generation，JIT lease
覆盖同步调用。这条路径没有使用 `emitRuntimeFragmentRefUnitIngressWrapper`：该通用
helper 有意拒绝 Apply region。组合入口仅编入测试目标，生产 CodeGenerator 与
module verifier 仍拒绝发布。native check 会读取由 Runtime 构造的 handle，不能
验证任意或已失效的 pointer；context 预检也有同样限制。私有状态配置将
成功／context／handle／target／未预期检查结果分别映射为 `0/1/2/3/4`；
只有精确的已知 native 结果获得相应名称，其他检查结果走未预期分支且不能进入
body。这些数值不是公开入口状态契约，body 也没有可恢复的执行失败状态。发布
仍须定义公开状态 ABI、符号／描述符映射及覆盖整次调用的代码 lease。
生成这个测试入口前，私有证明会将封闭的 Function declaration、可调用
TypeId／linkage／所有权契约，与源码签名、CFG 根参数和首个 Apply 的精确目标
交叉核对。篡改冻结的 linkage 或参数借用契约会使 JIT 物化失败。测试钩子现在还
生成一条不含指针的候选记录：独立 magic、版本、总长度、清零保留字、固定字段数、
带长度且有界的 module／package／function／Ref／Slot／entry 身份，以及明确的
调用、借用、结果和私有状态配置码，以及一个来自该精确函数 context-effect
固定点分析的字段。将字段改为 false 或伪造源码 effect 都会失败。校验器重新
检查封闭 CFG 与冻结事实。测试会
拒绝截断、额外字节、身份或约定字段变异、源码事实变化，以及通用 Native v1
导出行。此记录仅供测试；公开 descriptor、导出摘要和 loader 尚未使用它。
仅测试用的已装载入口视图在查找固定入口符号前，会对照冻结源码及生成该记录的 JIT
module 重新校验；有效记录配上未绑定该记录的 JIT module 也会被拒绝。视图持有代码
lease，每次同步调用再临时复制一份；夹具释放原始 JIT 引用后仍调用两次，释放视图后
确认 JIT lease 失效。两位由 Runtime 构造的借用 owner 在调用期间仍有效。第三次调用
改用仅测试用的 Ref 句柄副本（共享原有不可变 snapshot）和 parent context 副本；
夹具先释放原始 Ref 与 parent，再验证 snapshot pin 保持 generation 存活，释放 pin 后
确认 generation 结束。副本只从有效的类型化句柄建立，并拒绝覆盖非空输出；不接受
任意或失效的 opaque pointer。这只证明进程内来源绑定与寿命，不是 verified
artifact loader 或公开 Runtime lookup。
2026-10-05 的严格 CLANG64 与 Windows ASan／UBSan 构建均通过已装载入口视图的
聚焦 canonical 测试；生产 `luna` 目标构建、设计状态和 file-guide inventory 门禁
通过，`git diff --check` 无错误。后续借用 owner pin 扩展另行验证。这是本地切片
验证，不等于完整测试集或跨平台验证。
借用 owner pin 扩展也通过严格 CLANG64 和 Windows ASan／UBSan canonical 测试；
生产 `luna` 目标构建及两个文档门禁通过，仅测试用 helper 未进入生产 ABI。

#### 借用 Ref／unit 宿主入口候选契约（仅设计）

最小可发布形态是一次同步调用，逻辑签名为
`status(parent_context, borrowed_ref) -> i32`，源码参数为
`shared borrow RuntimeFragmentRef<S>`，返回 `unit`。宿主在调用全程保留两个
由 Runtime 构造的 owner；Ref 所有权不转交给 body。lookup 返回的固定 callable
须保留描述符和机器码直到调用结束。入口先检查 parent 和精确 Ref 目标，再进入
body。私有 `0/1/2/3/4` 配置只证明入口预检错误可以区分，不是公开状态 ABI。

| 候选契约事实 | 发布前必须验证 |
| --- | --- |
| 独立 magic、ABI 版本、结构大小和清零保留字段 | 读取入口字段前拒绝未知布局或版本。 |
| 函数 SymbolId、ContractId、TypeId、linkage 名称和已验证 registry 的所属 module 身份 | 对上一个封闭的导出函数与已验证产物；不能仅由原始符号地址推断 ABI。 |
| 精确目标 SlotId 和 Slot ContractId | 与冻结的 `RuntimeFragmentRef<S>` 目标及源码参数类型一致。 |
| 调用约定、parent／Ref 顺序的两个 pointer、共享借用模式、unit 结果与带版本状态域 | 与生成 wrapper 一致，并区分 context、handle、target 和执行失败。 |
| 装载后的入口视图：入口地址及 generation 身份 | 只经已验证、固定 module 的 lookup 取得；同步调用期间保留代码 lease。 |

现有通用 Native export 和 Runtime declaration 描述符未编码 Ref 目标或这套入口
ABI。Fragment 描述符对应 factory／environment／activation 执行，不能代替源码
函数入口。修改生产发布前须决定新类型化记录怎样附着在已验证 Moon／Native 导出上。
Moon Container 0.3 编码器仍拒绝 Apply region，module verifier 和 CodeGenerator
也仍拒绝发布源码 Ref。

现有 Native 验证链路确定了接入边界：`buildNativeLibrary` 从封闭的 declaration
table 构造各项 `NativeExportSpec`；`canonicalNativeExport` 只把 kind、flags、
SymbolId、ContractId 和 linkage 序列化供 proof export digest 计算；
`emitNativeLibraryDescriptor` 生成相应 v1 行。装载时，
`validateNativeDescriptor` 要求精确的 v1 行大小，并重新计算该摘要，然后导出项
才进入 generation。因此，仅在 Runtime binding 上附加 Ref 目标或入口约定，
不能得到现有 proof 的验证。不得原地扩展 v1 行。新的带版本类型化行必须从已验证
的冻结函数签名与 CFG 导出，同时进入规范化导出摘要和生成的 descriptor；loader
须检查字段有效性和身份歧义。重建后的 artifact digest 与 trust 记录便能约束映像
及其导出摘要。`stageVerifiedNativeGeneration` 当前只把 symbol／contract／entry／
kind／flags 复制到 `GenerationBinding`；宿主调用前，类型化事实还须传到固定的
lookup 结果。generation 已保留已验证库的 lease，但调用者须在整次同步调用期间
持有 `PinnedBinding`。

2026-10-06 实际以 `-t native` 构建导出且触达 Runtime Slot 的函数时，MoonIR
verifier 因缺少 runtime-aware 公开入口 ABI 而拒绝。语义与 Native artifact 回归
现固定了这项拒绝；Native 测试还要求不产生库文件或 trust 记录。现有 proof-v1
callable 测试只覆盖无参 `i32` 函数，不能验证 Ref／unit 入口。带版本的类型化导出
路径确定前继续保留此门禁。
2026-10-06 当前 CLANG64 工作树随后重建并以四个 worker 在 71.18 秒内通过全部
77 项本地非硬件 CTest；这是更广的回归证据，不是已验证 Native Ref／unit 导出或
发布批准。
隔离的 WSL Arch Linux Clang／LLVM 22.1.8 构建完成全部目标，并以四个 worker
在 67.15 秒内通过本地 Linux 完整 CTest 76／76 项。首次运行 75／76，唯一
失败的 frozen ecosystem baseline 检查是因为 WSL Git 未继承 Windows 系统的
`core.autocrlf=true` 设置，将子工作树的 CRLF 检出误判为修改。该设置下两个
子工作树均干净；仅对测试进程设置 Git 配置后，失败项与整套复测均通过。
这是本地 Linux 证据，不是远程平台 CI。

#### Native 类型化导出边界核查（2026-10-06）

当前 Native v1 导出行包含声明种类、callable 标志、SymbolId、ContractId、链接名和
裸入口地址。规范化导出摘要覆盖无指针字段，artifact 摘要绑定二进制字节，loader 核对
行结构和身份；但这些路径均没有调用签名、
Ref 目标或 Fragment context effect。`stageVerifiedNativeGeneration` 将该行继续
作为无类型化 `GenerationBinding` 传递；codegen 放入 callable 行的仍是生成函数体，
不是 Ref 宿主 wrapper。结构化 verifier 已拒绝依赖 context 的导出函数，Native 夹具
要求此类输入在产生库或 trust 记录前失败。

私有的双参数 `i32(parent_context, borrowed_ref)` wrapper 及其仅供测试的状态映射，
不能通过该 v1 行发布。它的无指针记录核对冻结签名／Slot 身份及 context effect；
私有 loader 还要求保留的 JIT module 携带完全相同的记录字节。该记录没有 CFG 或代码
摘要，单靠它不能证明分别构建的 artifact 具有相同函数体。

下一项可独立审查的切片是带版本和独立入口 ABI profile 的类型化导出契约：先定义双
参数调用约定、状态值、parent context 与借用句柄的寿命、Ref 目标身份和 effect 字段，
再生成可调用 descriptor。全部字段必须进入规范化导出 proof，并绑定已验证 wrapper；
仅给 v1 callable 行增加 profile 位并不足够：现有宿主调用者只检查 `CALLABLE`，然后
把裸地址转成 `i32()`；Ref 入口必须只能经强制核对其精确 profile 的 lookup 获取。
loader 校验及带 generation pin 的类型化 lookup 要拒绝字段缺失、版本混用和身份不符。
在整条链和 Container wire 规则通过前，Native v1 保持通用入口，源码 Ref 导出门禁
继续关闭。

Native v1 descriptor 生成器现于创建 registry 前，独立核对请求行是否对应已生成
模块中的公开 export，以及精确的 SymbolId／ContractId、声明种类、链接名和 callable 标志。即使
上游绕过结构化 verifier，它也会拒绝源码声明需要 Fragment context 的 callable。
canonical 回归先提供已生成但未公开的 `main`，再在生成后伪造 export 与 context
effect，两次均被拒绝；正常 Native artifact 生成仍通过。Windows CLANG64 与 WSL
Arch Linux 的 canonical、Native artifact 聚焦测试通过；Linux 完整 CTest 以四个
worker 在 75.62 秒内通过 76／76 项。这是 v1 的 fail-closed 门禁，不是类型化 Ref
入口或发布候选 CI。

Native artifact 门禁现在还构建带公开 `RuntimeFragmentRef<Slot>` 参数的 package，
要求 MoonIR 报告 Ref wire／ingress 未实现，并确认没有生成共享库或 trust 记录。
它将源码 Ref 发布与依赖 context 的 Slot 入口分开检验；当前由 verifier 先于后备
codegen 门禁拒绝。Windows CLANG64 和 WSL Arch Linux 的 Native artifact 聚焦
CTest 均通过。

写入 Native v1 callable 地址前，descriptor 生成器现在还会核对冻结 Function 的参数／
返回 TypeId、源码声明，以及实际已定义 LLVM 函数的类型和 C 调用约定。canonical
回归在 codegen 后同时篡改源码参数和冻结函数类型，未改变的 LLVM 函数体不能按
伪造签名发布。Windows CLANG64 与 WSL Arch Linux 的 canonical、Native artifact
CTest 均通过；Linux 完整 CTest 以四个 worker 在 74.36 秒内通过 76／76 项。
此项核对的是编译器内部一致性，不告诉外部 v1 调用者该用什么签名。

首个**并行 Native v2 descriptor／query** 切片已对经过验证的无参 `i32` 导出实现。
v1 行、查询符号和 proof 导出摘要保持不变。生成器先核对封闭的源码签名及实际已定义、
遵守 C 调用约定的 LLVM `i32()` 函数，才加入 `C_I32_NOARGS_V1` 行。加载器核对
v2 header、身份、profile、规范化行摘要及与已验证 v1 行的精确对应；现有 artifact
proof 摘要覆盖全部 v2 metadata。`VerifiedNativeLibrary::callI32NoArgs` 在库
持有期间执行类型化入口，不向外返回新的裸函数指针。独立消费者核对两版 query 与
摘要；重新封装 proof／trust 的 v2 摘要变异在 Windows CLANG64 与 WSL Arch Linux
上被加载器拒绝。Native generation 解析现在只把已验证的 v2 profile 写入独立的
`GenerationBinding.entryAbi` 字段。带 profile 的 requirement 精确匹配入口；
`PinnedBinding::callI32NoArgs` 调用期间由 generation 保留已验证库的 lease。
load-once、activation 和 switchable requirement 均核对 profile 稳定性。
旧式四字段 requirement 仍按身份／种类／flags 匹配，但显式要求无 profile 时
不能匹配类型化导出。早期改名 v2 query 的合成制品在 macOS 失败（消费者退出码 24），
且依赖改写已链接动态库字节，因此已删除该夹具。
另一个从纯 v1 C 源码独立构建、拥有自身 proof／trust 的共享库，也通过独立
proof oracle、v1 loader／调用及无 profile generation 检查；因无 v2 query，
类型化 lookup 被拒绝。这个独立夹具现于三个 64 位 CI 平台验证可选 query 路径。
这是已实现的 ABI 实验，尚非冻结的公开 ABI 或 Ref wrapper 导出。
此项接入后的本地 CLANG64 与 WSL Arch Linux 完整 CTest 分别通过 77／77 和
76／76 项；该计数早于三项仅编译布局测试接入。

下列候选记录当前 profile 布局、摘要和版本规则。现有发布包与 CI 矩阵
覆盖 64 位 Linux、Windows 及 macOS runner 架构，没有列出 32 位发布目标。
[发布状态](ecosystem_release.zh-CN.md)记录修正后实施提交 `8aac3a0` 的 Linux、
Windows 和 macOS CI 均成功，包括 Native artifact 测试。公开 ABI 承诺前，
仍须让宿主使用方审阅后续 query 版本规则与指针寿命义务。源码 Ref 发布仍须等 context effect、精确 Slot／Contract 目标、
carrier 状态与所有权语义完成端到端封闭和核验。

#### Native v2 本地布局与版本候选（2026-10-06）

当前 64 位目标矩阵固定自然 C 布局：v1 导出／库记录分别为 48／64 字节，v2 分别
为 56／96 字节。v2 行中的 `entry_abi`、`symbol_id`、`entry` 偏移为 16、24、
48；v2 库记录中的 `export_count`、`exports`、32 字节摘要偏移为 48、56、64。
C 头文件对此作静态断言，独立消费者也会检查。对 i386 GNU/Linux、i686
Windows GNU 和 i386 Darwin 的 freestanding 仅编译探针显示：32 位 v1／v2
导出记录大小为 32／40 字节；v2 行的 `entry_abi`、`symbol_id`、`entry`
偏移为 16、24、36，v2 库记录的 `export_count`、`exports`、摘要偏移为
32、40、44。库记录的尾部对齐不同：所测 GNU/Linux、Darwin 的 v1／v2
大小为 44／76 字节，Windows 为 48／80 字节。可复现探针见
`tests/native_abi_layout_probe.c`；Clang CTest 会在每个配置的宿主上对三个
triple 仅编译，不使用 32 位链接器或运行时。本地 Windows CLANG64 与 WSL
Arch Linux 的三项布局 CTest 和文件清单测试均通过。接入这三项测试后，
完整非硬件 CTest 在 Windows CLANG64 通过 80／80，在 WSL Arch Linux
通过 79／79。这些结果
不证明 32 位制品生产、加载、执行或发布支持。若将来增加 32 位目标，须另行
完成端到端制品／运行时 CI，并明确决定 ABI／版本。

v1 proof／query 始终必需且保持不变。v2 query 可以缺席：此时 v1 binding 无
profile；若存在，则必须符合精确的 ABI 2、结构大小、保留字段为零、proof 身份
一致，且 v2 行是已验证 v1 行的子集，身份、flags、链接名和入口地址都一致。
同版本追加尾字段会被拒绝；更长记录须使用新的 query 与 schema 版本。
schema 2 只认 `C_I32_NOARGS_V1 = 1`，即遵守 C 调用约定的已定义
`int32_t(void)` 函数。未知 profile 或异常 v2 行拒绝整份已加载映像，不退回 v1。

v2 descriptor 摘要采用 SHA-256：先编码小端 `u32` 行数，再对排序去重后的每行
编码 `u32` 字节长度与原始字节。行内容是 `LUNA_NATIVE_EXPORT_V2\n`，再以 `\n`
分隔十进制 kind、flags、entry ABI、SymbolId、ContractId 和链接名，末尾没有
分隔符。进程内指针不进入规范化行；v1 整制品 proof 摘要绑定包含这些指针的
链接映像。若改行编码、摘要算法或加入新 profile，必须采用新的并行 query／
schema 版本；现有 v2 loader 会拒绝未知字段与 profile。
加载器要求 v1、v2 descriptor 字符串非空、有界、采用有效 UTF-8 且不含 CR、LF 或
tab。独立封装的 v1 测试制品若含非法 UTF-8 符号，离线验证可以成功，宿主加载则会拒绝。
同一独立 C 夹具也能生产 v2 query：有效的封装制品支持宿主类型化调用；另一个
v2 行结构大小过大的封装制品通过离线 proof 验证，却被宿主加载拒绝。该检查不依赖
Luna 的 LLVM emitter，覆盖并行 query 与精确大小规则。
同一 v2 夹具还在 `MoonRuntime` 销毁后调用固定的类型化 binding，验证 generation
的 module lease 独立于 runtime 对象保留代码。

在 MoonRuntime 中，binding profile 0 表示无 profile，profile 1 允许固定的
`i32()` 调用。`GenerationEntryAbiAny` 只能出现在 requirement，作为通配符保留
旧式四字段调用的源码兼容；显式 0 或 1 则精确匹配。switchable binding 会捕获
当前具体 profile，拒绝以后切换到不同 profile 的 generation。这些规则是本地
候选，并非已发布的稳定外部 ABI。

上述私有类型化行编码与校验只完成了本地源码到记录的核对。Moon Container 的
Function code 记录已保存 `requiresFragmentContext`，module verifier 会从 CFG
重新推导它，但 Container 编码器仍拒绝 Apply graph。冻结的 `DeclarationRecord`／
规范契约与 Native v1 导出行均未包含该 effect；structured verifier 还会拒绝
要求 Fragment context 的导出函数。因此下一道边界是公开状态映射，以及针对依赖
context 的源码导出函数一同调整的 verifier／export 规则。其 effect 和 Ref 目标
须进入封闭的导出 metadata；随后一起扩展 v2 profile、规范化哈希、loader 校验
和固定的 Runtime lookup，同时让通用 v1 行保持无类型化入口语义。

### 下一步源码 Ref／apply 完成门（更新于 2026-10-08）

以下是剩余源码功能的建议实施顺序，不是公开 ABI 决议或稳定版发布批准：

1. **闭合控制流与清理证明。** 私有两层、仅经 Jump 转换的路径已有单或双借用 Ref、
   相同或不同精确 Slot 的 owner 栈证明与 JIT 证据，包括 outlined 回调内的内层
   Apply。当前源码 builder 使 Apply 入口和正常出口经 Jump 转换；在源码 lowering
   产生非 Jump 跨区边前，继续明确拒绝它。受限的 `Result<i32, i32>` `?` 路径已有
   先内后外的 CFG 证明和 Ok／Err 私有 JIT 证据，包括将一个仿射源码错误经冻结
   `From` 方法转换为标量并由源码 `Drop` 清理。双节点拥有型转换参数也已在
   Windows 与 Linux 的 ASAN JIT 运行中证明两条标量返回分支均外层先于内层
   Drop，其中正值路径将整个 owner move 到分支局部绑定。部分字段转移、向宿主
   转移所有权及更宽的转换仍需证明。手工构造的 canonical CFG 现验证完整、互不
   重叠的字段清理记录、一条底层 allocation 清理记录，以及一个字段转移后的有效
   return 清理列表。下一步从拥有型源码 struct 生成这些记录，使 return 与作用域
   退出义务具备字段敏感性，再用正向 JIT 夹具证明字段 Drop 与 allocation 释放的
   实际顺序和恰好一次执行。
   单／双字段资源 Err 与单字段资源 Ok 已由私有
   wrapper 在有序 context 退出后观察并销毁；对应标量分支不执行 Drop。三个
   struct 以内的一条所有权链现有递归清理及有序 Drop／deallocation 证明。
   两个独立拥有字段组成的三节点有界分叉，现也在两端普通及 ASAN 聚焦测试中
   验证 `?` Err 和注入失败的有序清理。更大或更宽的拥有型字段图仍在证明范围外。
   仅测试用的宿主交接入口已验证空 owner
   cell 预检、tag／owner 提交、借用 Ref pin 失效后单独保留 JIT lease，以及三层
   链的显式恰好一次 Drop。
   注入的 body 返回后失败现证明未提交 owner 的清理；公开宿主所有权 carrier 与
   生产失败状态仍需契约及可执行证明。发布可返回失败的
   路径前，须明确可恢复的派生／分派失败清理。
   每条准入的出口都须在 apply 局部清理之后、借用的
   Ref owner Drop 之前，按内到外恰好一次释放有效 context；其余路径继续拒绝。
   独立捕获 Windows LLVM 20.1.8 RuntimeDyld 间歇性 COFF 重定位失败所对应的
   精确 JIT 调用。直接切换 JITLink 又遇到不同的 `.pdata` `Pointer32` 范围失败；
   下一步在隔离构建中验证保序 section allocator 或较新版 LLVM，重复 ASAN
   测试后再将 sanitizer 结果视为稳定证据。
2. **冻结可发布的宿主边界。** 明确带版本的 Ref 入口 carrier、返回 carrier／状态、
   精确 Slot／Contract 校验、借用寿命、所有权提交点及失败清理。现有私有 wrapper
   与 opaque 内部类型只是验证证据，不是公开契约。最小候选是一个同步共享借用 Ref
   参数和 unit 返回，由返回状态的 wrapper 在进入函数体前校验精确 Slot 和有效的
   parent context。仅测试用的组合入口现已用真实正确／错误目标 handle 和空
   parent 执行两项检查；通用入口 helper 仍排除 Apply region。发布前须冻结候选的
   类型化记录与状态版本，把它附着到已验证导出，并将调用期代码 lease 接入公开
   lookup 路径，再同步修改
   structured verifier、导出符号
   metadata、CodeGenerator 和 dropGlue；
   现有 check／transfer／drop 测试覆盖 native carrier 行为，却不能证明源码入口
   已发布。
   独立的仅测试用 tag／owner-cell 与 JIT Drop 入口现已验证候选提交及 lease 规则。
   发布资源返回 carrier 前须把 owner 与 JIT lease 合并为一个寿命单元，并定义
   状态与布局版本、JIT 关闭行为，以及采用此
   提交前清理的真实失败条件。注入的 body 返回后失败已有可执行清理证明，
   此实验仍不构成公开 ABI。
3. **接通 verified 产物路径。** 为已准入的控制流完成 compiler dropGlue 和生产
   codegen，再于 module verifier 及 Moon Container 编解码中定义、验证 Ref 类型
   和 Apply region 的 wire 规则。无效或旧产物必须在发布 generation 前整体拒绝。
4. **通过真实双包执行门。** 从 verified generation 导入宿主创建且固定 generation
   的 Ref，在显式 parent context 中执行源码 apply；先打通借用入口，再完成拥有型
   参数／返回转移。覆盖名义／contract 错配、入口失败、正常／提前退出、重复 apply、
   清理与 pin 释放。完成此门和剩余控制流用例后，`source-ref-apply` 才可脱离
   `implementation-open`。

性能验收、持久证据存储和稳定版发布批准仍是 v1 验收快照中的独立完成门。

## 宿主控制的发现与注入

目标指向 exported Slot 的 exported Fragment，凭名义关系成为候选；metadata 不授予
候选资格。已验证产物发布只含数据的 `FragmentOffer`，至少携带 FragmentId、目标
SlotId/ContractId、执行/factory 契约、环境布局、generation identity 和保留的策略
metadata。

已实现的 v1 查询作用于显式传入的单个已验证 generation 和精确 Slot／Contract 要求：

```text
snapshotRuntimeFragmentCandidates(generation, slot, ...) -> 固定 generation 的快照
```

结果对该 generation 完整且确定，保持不可变并固定 generation。宿主自行追踪已加载的
generation，可以按自身策略合并快照。跨 generation 查询或全局索引不属于 v1；未来
可另行考虑便利 API。

发现、策略与执行彼此分离：

```text
已验证 FragmentOffer
    -> CandidateSnapshot
    -> 宿主策略选择 None、One 或 OrderedChain
    -> 构造 RuntimeFragmentRef 并一次性验证契约
    -> 不可变 BindingSet
    -> safe point 激活
```

Runtime 不替宿主选择胜者，不按加载顺序排序，也不会在新候选出现时自动注入。加载新
generation 可以通知宿主 catalog snapshot 已变化，但只有宿主能决定是否构造并激活新
BindingSet；已有引用继续持有旧 ModuleLease。

Slot dispatch 只读取已解析的 BindingSet。普通热路径不运行反射、metadata 过滤、候选
枚举或完整 ABI 验证。

## Metadata 与 sysmeta

候选成员关系来自已验证的 Fragment-to-Slot 名义关系。Metadata 是宿主用于过滤和排序
的不可变强类型策略数据。其 schema 对候选接口公开时，attachment 直接存入
FragmentOffer；它不会让其他声明自动获得 callable 或 executable 能力。

编译器拥有的 sysmeta 记录全部安全与 lowering 事实：SlotId、ContractId、目标关系、
unit/single-shot control、续体使用、环境布局、ownership、ABI 和 generation 生命周期
要求。用户 metadata 不能伪造这些事实。

跨 C++／C 边界的 Runtime identity key 除 CR／LF／tab 外，也拒绝内嵌 NUL，避免转成
null-terminated ABI 名称时静默改变 module、声明、Slot contract 或参数 layout 身份。
Staging 在初始化前拒绝非法 generation key；候选发现、activation、分派（包括 None）
和 override 也拒绝有歧义的 Slot key。
这些是 C++ 身份检查；C ABI 名称止于第一个 NUL，native 调用方不得先截断一个未经
验证的 C++ ID，再调用该 ABI。

Runtime-retained metadata 只需要最小 owner identity anchor，不得把 owner 提升为 callable
function、executable fragment、开放 slot 或 Runtime API。该边界已在前端、MoonIR verifier
与 Runtime descriptor 发射中落实：metadata owner 可以保持 compile-time retention，其
descriptor 的 callable flag 为 0、entry 为 null。

## 验证边界

Artifact verification 证明每条 FragmentOffer 与 sealed MoonIR 及 artifact proof 一致。
Binding 构造阶段一次性验证精确 SlotId、ContractId、环境布局、declaration kind、ABI
版本和必需 entry flags。成功构造的 `RuntimeFragmentRef<S>` 只能安装给 `S`；普通函数
指针以及目标为另一同形 Slot 的 Fragment 都会被拒绝。

Fragment 续体由编译器拥有，不可伪造、不可逃逸，并且每个 activation 最多消费一次。
未来 ordered chain 中的 `resume` 前进到宿主选择的下一个 Fragment，最终进入 base
continuation；隐式链接或加载顺序不得决定处理链。

Resume 失败在有效 activation 内保持失败，包括重复或递归使用同一个 activation。
返回 void 的 native execute thunk 不能仅靠忽略负值 resume 结果隐藏失败：thunk 返回后，
外层 dispatch 报告执行失败。下游 chain 诊断保持完整，每次新调用都使用全新的 activation
状态。这既不许可 handler body 重入，也不回滚 native 副作用。

### 阶段 3 Runtime ABI 基础

阶段 3 的首个切片冻结 C-compatible Fragment descriptor，其中包含 FragmentId/ContractId、
精确目标 SlotId/ContractId、factory contract、环境布局、factory/destroy 函数与 execution
thunk。thunk 接收编译器拥有的 opaque activation，而不是宿主可构造的 continuation 表示。

C++ 宿主层从已验证 generation binding 构造 move-only `RuntimeFragmentRef`。构造过程先
固定该 binding 所属 generation，再一次性校验全部 identity 与 layout 并调用 factory。
同步 native factory 回调清空或替换输入 binding，不会改变新引用选定的 generation。
拒绝产物的清理也保留原 pin；成功则将该 pin 交给引用。factory-owned 环境会先于
generation lease 销毁；borrowed 环境必须携带自己的显式 lease。

引用 reset 在 owned destroy／borrowed environment lease 释放前摘走旧状态，并保留原
generation 直到这些清理完成，因此嵌套 reset 看到空引用。同步清理回调可重新绑定仍
存活的引用，外层 reset 不会清除该新值。移动赋值先安装 incoming 状态，再清理旧
状态，同样保留回调修改。清理不得抛异常、销毁仍在使用的对象或复活正在析构的对象。
这些 native 宿主生命周期规则不扩大 Luna handler body 重入范围，也不改变 v1 ABI。

环境构造检查实际地址是否满足声明的对齐，而不只检查 alignment 字段。被拒绝的非空
工厂产物在发布前销毁一次，borrowed 环境不会传给 Fragment 的 destroy 回调。Slot activation 以及 C++／C
分派都在 Fragment 或 base continuation 执行之前拒绝非法或错位参数，包括宿主策略
None。空载体固定使用 size 0、alignment 1、null data。这些检查不证明分配边界或
生命周期，也不改变 v1 ABI。

阶段 3 的第二个切片实现了源码构造分离：
`fragment name[环境](Slot 参数) for slot` 与 `apply name[实参] { ... }`。apply 实参只在
Apply region 入口求值一次，每个 Slot activation 借用所得 binding。语义分析会隔离 Fragment
正文与调用者局部作用域，因此遗漏环境不再退化为意外闭包。环境作为冻结的 structural
Record TypeId 进入 MoonIR。Apply 现在拥有 Copy 与 affine 字段，activation 只借用这些
字段，所有正常退出与外层逃逸 CFG edge 都携带相应 cleanup。linear 字段仍会被拒绝，因为
这种可复用环境模型无法证明恰好消费一次。

第三个切片在 Runtime ABI 中明确拆分发布与 retention。导出的 Slot/Fragment 行携带
`PUBLIC_CONTROL`，同时仍可保持 compile-time retention；该标志本身绝不表示 callable 或
Fragment executable。导出的 Fragment 必须指向导出的 Slot。canonical declaration/container
模型现在保存并验证精确目标 Slot 引用与冻结环境 TypeId，因此 JIT 重新加载时不需要从用户
metadata 或仅存在于前端的 recipe 恢复这些安全事实。

宿主 Runtime 也已提供不可变、按 SymbolId 排序、固定 generation 的候选快照。查询按精确
SlotId/ContractId 过滤，并且只纳入同时具备 public-control 与 executable-fragment 能力的绑定；
它不排序优先级，也不激活候选。

第四个切片为每个 Slot 冻结独立的 structural argument Record TypeId，并通过 Fragment 行与
已验证容器保留该精确记录。Fragment descriptor 现在包含参数布局。Runtime 拥有 opaque
activation；只有 SlotId、ContractId、layout、size 与 alignment 全部精确匹配时，生成代码才能
读取参数，而且续体最多消费一次。作为有意收窄的首版 executable ABI，exported Slot 当前只
接受 Copy 参数契约，exported Fragment 当前只接受 Copy 环境；静态组合仍支持 affine 环境。
move-only 值跨宿主边界需要明确的转移/drop 协议，不能用不安全的位拷贝近似。

第五个切片把每个 exported Fragment 物化为内部普通函数。该函数复用同一条已经验证的静态
`apply`/Slot/`resume` CFG 组合路径，容器保留的 `runtimeEntry` 引用负责使 helper 可达。LLVM
按需发射有状态 factory/destroy、负责校验并打开 opaque activation 的 execute wrapper，以及
`FRAGMENT_EXECUTABLE` descriptor。verified loader 在发布可执行候选前，会把 descriptor 与
冻结的 Slot、参数和环境布局逐项核对。无捕获与有状态 ABI 均有覆盖，其中包含完整 generation
加载和 single-shot resume 的端到端测试。

第六个切片实现阶段 5 的宿主激活边界。宿主可以把已经验证的 `RuntimeFragmentRef` 消费为
不可变 BindingSet，每个精确 SlotId/ContractId 最多选择一个胜者。`MoonRuntime` 只有拿到来自
同一 runtime 的 fresh `SafePoint` 才会原子发布该集合，并向 dispatch 返回 pinned snapshot。
替换 active set 不会改指向或使旧 snapshot 失效。每次分派自身保留已选快照，直到所有
handler 返回；即使同步 native 回调清空或替换调用者的公开 C++ BindingSet／context 句柄，
owned／borrowed 环境与 generation lease 在正常完成、逃逸及失败路径中仍保持存活。
这不允许复用已释放的 opaque pointer，也不允许并发修改同一个 C++ 句柄。
dispatch 不执行发现、metadata 过滤或
descriptor 重新验证：它只做一次精确 Slot 查找、核对本次参数布局，然后执行选中的 Fragment；
宿主策略为 `None` 时则直接调用 base continuation。

第七个切片在同一不可变表示上加入显式 ordered chain 与局部 override。chain 构造器保留宿主
对每个精确 Slot 给出的输入顺序，即使 candidate catalog 的 SymbolId 顺序不同也不会改排。
每个 `resume` 同步进入下一个 Fragment，最后一个进入 base continuation，随后控制流按嵌套
逆序返回各 Fragment 的 post-resume 代码。局部 override 共享已固定的 Fragment reference，
但只替换或屏蔽一个精确 Slot；它不会修改 base snapshot 或全局 active BindingSet。

第八个切片冻结 compiled-code dispatch 边界。宿主把一个 pinned BindingSet 转成 opaque
`RuntimeFragmentExecutionContext`，并在执行入口显式传入这个 capability。稳定 C ABI 接收该
context、一个精确 Slot identity 与参数布局，以及编译器拥有的同步 continuation。continuation
若执行外层 `return` 或 `?`，ABI 会返回独立的控制结果；生成的 Fragment helper 必须继续传播
该结果，并跳过 `resume` 后的代码。context 在整个调用期间固定其 BindingSet，但不持有
`MoonRuntime*` 回指，不访问 candidate catalog，也不查找进程全局 current Runtime 或 TLS。

### 动态 dispatch lowering 决策

对执行上下文的传递曾比较三种方式：

- 进程全局或 TLS current Runtime 不改变函数 ABI，但嵌套宿主、并发 Runtime、任务迁移和局部
  override 都会依赖不可见可变状态，因此否决。
- 给全部 Luna 函数增加隐藏 context 参数易于验证，但会让不可能触达 runtime Slot 的程序和
  调用链承担代码体积与调用约定成本，因此不作为默认方案。
- effect-directed propagation 只给其传递调用图能够触达 exported、可动态 dispatch Slot 的函数
  增加隐藏 context。代价是新增 call-graph effect 与 context-aware indirect-call ABI，但保住了
  静态路径的零成本；这是选定方案。

编译器集成按以下顺序推进：

1. exported Slot 在没有静态绑定时，不再被 sealing 直接消除，而是成为名义化 runtime Slot
   terminator；它携带 Slot declaration reference、冻结参数 Record TypeId、实参、continuation
   入口和 escape outcome。private Slot 在没有静态 `apply` 时仍擦除成 identity。
2. 在 direct call graph 上求不动点并验证 `requires_fragment_context` effect。function value 与
   exported entry 必须编码同一个 ABI 事实；loader 不得再从用户 metadata 推断它。
3. 仅受影响的内部函数获得隐藏 execution-context 参数。runtime-aware exported descriptor 提供
   显式宿主入口；若普通 callable export 的实现传递地需要该 capability、其公开 ABI 却不携带，
   编译器必须拒绝。
4. outline Slot continuation 及其 live Copy frame，按冻结 Slot record 打包参数，然后调用
   `luna_runtime_fragment_dispatch_v1`。`CONTINUATION_ESCAPED` 选择已验证的外层 return/error edge；
   负 dispatch 结果进入 Runtime error boundary。
5. 静态 `apply` composition 始终先于这一步 lowering。词法绑定已知时，现有内联 CFG 仍是唯一
   语义，不发射 runtime dispatch、BindingSet lookup 或 execution-context 依赖。

## 实施阶段

1. 引入统一的 `slot`、`fragment ... for`、`resume;` 前端表面；在删除旧内部
   FragmentKind 分支期间，先复用现有 single-shot 静态管线。
2. 删除 `interceptor`、`context`、`abort`、`many`、声明 default 以及 Slot/Fragment 对
   `runtime` 的使用；在 sysmeta 与 MoonIR 中形成唯一 canonical control 模型。
3. 增加显式 Fragment 环境、factory/execution descriptor、`RuntimeFragmentRef<S>`、
   一次性验证、cleanup 和 ModuleLease。
4. 发射已验证 FragmentOffer，并向宿主提供不可变、generation-pinned 的
   CandidateSnapshot 语义。
5. 增加宿主创建的不可变 BindingSet 与 safe-point activation，确保候选发现和 metadata
   策略不进入 dispatch 热路径。
6. 在同一 binding 表示上增加确定的 ordered chain 与局部 `apply` override，随后建立性能
   和 artifact cost 门禁。
7. 增加显式 execution context、保留动态 Slot terminator，并只沿动态受影响调用链传播该
   context capability。

截至 2026-09-25，阶段 1 至 6 已完成。AST、类型系统、Sema、sysmeta、MoonIR、lowering、
verifier、JIT 与 AOT 路径现在只暴露一个 canonical single-shot Fragment 模型。0.3 容器仍在
原偏移保留两个 wire position，但只写入 canonical 值，并在解码时拒绝 legacy 值；它们不再
作为语义字段出现。Runtime ABI/reference 基础，以及编译器侧显式 Copy/affine
环境所有权、借用、转移检查和 cleanup lowering 已实现。public-control descriptor 发布、
容器保留的目标/环境事实与候选快照语义也已实现。Slot 参数布局保留与 Runtime-owned opaque
single-shot activation 已完成。编译器生成的隐藏入口、factory/destroy/execute thunk、可执行
descriptor 发布和 verified generation 加载也已完成。宿主拥有的不可变 BindingSet 构造、
safe-point activation、pinned dispatch snapshot、None/One dispatch、确定性 ordered chain 与
不可变局部 Slot override 也已完成。artifact-cost 门禁现在证明 exported executable Fragment
会物化 descriptor/factory/execute ABI，而 private static composition 不会保留这些成本。显式
execution-context C ABI 与 continuation escape 传播也已完成。阶段 7 现在会把每个未静态绑定的
exported Slot invocation 保留为经过验证的 `RuntimeSlot`
terminator，其中显式携带确切 Slot declaration reference、冻结的参数 Record TypeId、已打包参数、
continuation 入口和完成边。静态绑定的 `apply` 仍优先完成 composition，未绑定的 private Slot 仍直接
擦除为 continuation。container codec 与 projection 会保留这些 terminator 事实。
编译器还会在精确 direct call graph 上对
`requires_fragment_context` 求最小不动点，将结果写入 function code row，并由 verifier 独立重算，
拒绝伪造或过期 summary。该 effect 现在会在受影响的内部 LLVM function 上物化为一个前置 opaque
context 参数，精确 direct call 会转发调用者的 capability；不受影响的函数保持原 ABI。普通 export、
`main`、extern、kernel 与 function-value 物化不能静默跨越该边界；需要 context 的
`runtime fn` 是已支持的宿主入口，context-aware indirect call 仍未支持。嵌套 closure body
仍按保守方式计入 effect。
需要 context 的 `runtime fn` 现在就是显式宿主入口：它复用现有 Function descriptor 与 generation
binding，并携带 `FRAGMENT_CONTEXT`，表示入口在源码参数之前接收 opaque execution-context
pointer。`export` 仍只表示普通外部可见性，不会隐式获得该 ABI。跨包 Slot 调用和 exported
Fragment 指向依赖包 Slot 时，均使用该 Slot 的确切声明身份，并要求所属依赖包将其公开为
控制点。verifier 现在核对根包 export 的声明归属；可执行声明存在时还核对其公开标志，
伪造 export 行不能把私有或外包 Slot 变为运行时目标。外包 Slot 的 packageId 还必须与
稳定声明 ID 的归属一致，不能只改包名和依赖行冒充。端到端宿主测试现已分别编译 Slot
所属完整包与插件包，加载为不同的 JIT generation，
从插件选取该 Slot 的候选，并通过不同返回值区分选中 dispatch 与未绑定续体。runtime 销毁后
execution context 仍固定插件 generation；释放 context 后 generation 随之释放。宿主包中未被本包
`select` 调用的 exported 编译期 selector 现于声明阶段分类，并从 MoonIR 擦除。
另有 verified container 回归覆盖本包公开 Slot：codec 保留 declaration／contract／export
行，但不重建前端 `SlotDecl` 对象。验证器在缺少该临时对象时接受本包确切 Slot export；
对象仍存在时继续核对其公开标志。缺失、错误 kind／contract 的公开行与外包 import／
re-export 冒充仍被拒绝。完整 encode／decode／load 测试覆盖生成的 Copy factory、None／
One、resume／discard、capture 回写及 return 逃逸，且在 Runtime 销毁后继续执行。
上述跨包源码到 JIT 测试与产物加载是两条不同证据。独立的小型 host/plugin workspace
现已覆盖 verified container 缺口：两包分别编码容器，使用已验证 generation 适配器加载，
而非直接注册 JIT。Slot 公开事实仍持久化在所属包根 `Exports` 中；所属包完整解码验证后
签发不可变 `localSlotPublication` 快照。消费者的 decode、stage 与 load-once 接收显式
`SlotPublicationDependencies`，核对 owner/直接导入、target/layout、精确身份、结构类型
与参数布局。源码复合包只在 concrete projection 校验时使用临时编译器事实，不把它写入
消费者 exports；缺少所属包独立证据仍 fail closed。空/重复、私有 owner、变化契约、
无关 owner 与另一 target 的证据均被拒绝，且不部分发布输出或执行 initializer；
load-once 缓存命中也不能绕过验证。消费者重新编码后的字节完全一致，证明未改变
wire format、ContractId 编码或 Runtime ABI。证据快照不引用可变的解码后 export 行，
消费者也不能签发自己导入的 Slot 的本包公开证据。
正式 `luna build <package> -t moon` 打包入口也已覆盖。完整的 target-bound roundtrip
现位于 `encodeContainer` 内，使用仍可用的已验证源码 projection 依赖事实；这些临时
事实不返回调用者。CLI 不再执行第二次无依赖上下文的 decode。CLI 门禁将 host/plugin
各自生成两份实际文件并比较 hash；产物消费者仍必须提供独立解码的 owner 证据。

```sh
luna build tests/fixtures/runtime_fragment_container/host -t moon -o build/fragment-host.moon
luna build tests/fixtures/runtime_fragment_container/plugin -t moon -o build/fragment-plugin.moon
```

回归在 Runtime 销毁后执行 64 次真实 host/consumer None/One 调用。这是功能性产物证据，
不是延迟 benchmark 或发布批准。下方的 compiled-plugin 对比探针使用这些显式 owner
产物，将 setup/discovery 放在 dispatch 计时之外。产物信任/认证、依赖取得与
候选选择策略仍由宿主负责，不增加隐式加载、激活或热路径 catalog。
当前 lowering
会打包冻结的 Slot 参数 record，并通过一个同步栈
frame 调用 `luna_runtime_fragment_dispatch_v1`；frame 携带 execution context、return storage 和
指向 live capture 的指针。callback 把 capture 暂存进 typed local、执行 outlined blocks、
回写修改，并报告正常完成或外层 `return`/`?` 逃逸；dispatcher 随后选择 completion
edge 或返回逃逸值。continuation-local 资源的 canonical cleanup edge，以及 `?` 所需的 Result
switch/case binding，都会在 callback 报告逃逸前执行。continuation 可调用另一个触达动态 Slot
的函数，同一 context 会显式转发。词法嵌套的 Slot 会递归 outline callback、共享 context
与 return storage，并回写跨层 capture。外层 affine/resource 在正常完成或内层逃逸时都按
封存的 cleanup 处理；所有权分析还会把 Fragment 不调用 `resume` 视为一条可继续路径，
因此 Slot 后续路径也保留准确的资源清理。新增的非计时型
结构门禁要求每个动态 Slot site 只生成一次 dispatch call，静态组合
不携带 Runtime 选择机制。

### 静态环防护与续体嵌套

截至 2026-09-26，Sema 与 CFG builder 独立拒绝直接或相互递归的静态 Fragment 展开，
避免无限递归。回归覆盖源码诊断，以及在语义分析后注入递归 MoonIR body 的情况。
base continuation 中有限的同 Slot 嵌套仍合法，包括词法绑定 override。CFG verifier
仅允许从外层 Fragment 进入直接嵌套 Fragment 的已记录入口块，伪造跳转到内层
非入口块仍会被拒绝。Runtime C ABI
测试用同一 pinned context 和已选 chain 嵌套分派同一 Slot，核对独立 single-shot
activation 状态，并验证内层续体逃逸能穿过内外两条 chain。任意 handler body 的
runtime 递归仍不属于已冻结的 `TBD-SF008` 保证；这些检查不引入 runtime 深度限制，
也不替宿主选择自动抑制策略。

### 发布 handler 的执行边界

发布 handler 的边界现在独立于本包使用情况进行检查：exported Fragment body 即使
没有本地 `apply`，也必须通过语义与独立所有权分析。局部 linear 状态在所有正常结束路径
上都必须消耗，冲突借用与重复 free 在前端被拒绝，仅宿主选择的候选也保留隐式 affine
清理。独立所有权检查使用不携带调用者资源、正常结束的 opaque 续体；实际静态 apply
另外检查真实续体的路径。封存后 verifier 依据独立重算的 direct-call
最小不动点，拒绝需要 execution context 的 Fragment runtime entry，因为 v1 的公开
execute wrapper 无法传递该 capability。直接与传递依赖均由 `luna check` 在 codegen
前拒绝；伪造 helper 的 context-free summary 不能绕过检查。静态绑定的 exported Slot、
擦除的 private Slot 以及私有静态 Fragment 组合仍合法。擦除的 private Slot body 使用
词法 region，而不是 suspended continuation region，因此可在 handler 内执行并访问
其局部变量，不需要放宽真实 Fragment continuation 的边界。Runtime handler context
传递及其重入策略仍需明确的 ABI／设计决定；没有新增隐式 context 来源。

[SF008 有界契约证据](slot_fragment_contract.zh-CN.md)集中整理上述边界，以及嵌套
override／None、静态 discard／return 门禁；记录的是已实现行为，不是稳定版发布授权。

### 并发宿主证据

并发宿主证据见 `luna.runtime-fragment-concurrency`：四个 reader 在仍存活的续体中
跨越 96 次 None／One／chain 原子发布，覆盖正常完成、逃逸、失败隔离；Runtime 销毁
后，reader 各自持有的同一 pinned context 状态副本再分派 1024 次。独立目标在
ASan/UBSan 或 Linux 独立 TSan 作业中同时插桩两份 Runtime 源码，不插桩 AOT archive
或 LLVM/ORC。不可变 native 环境与逐调用参数不授予任意插件线程安全，也不新增 Luna
跨线程 API。复现方式见[测试指南](testing.zh-CN.md)。

### 可复现的运行时成本探针

`runtime-fragment-benchmark` 是显式构建的微基准，不参加默认构建或计时型 CI 门禁：

```sh
cmake --build build-perf --config Release --target runtime-fragment-benchmark
./build-perf/runtime-fragment-benchmark 100000 64
```

Windows 可执行文件带 `.exe`，多配置构建则位于 `Release` 子目录。第一个参数是每项
迭代次数，第二个是同一 generation 的 Fragment 行数（至少 4，至多 4096），其中
4 行目标指向被查询的 Slot。建议分别运行 4、64、256 行，并记录平台、编译器、构建
类型与每项 ns/op；不要跨机器直接比较绝对时间。输出区分候选快照查询、引用加 BindingSet
构建、四引用 chain 构造、局部 None override 构造、safe-point 激活加 pin，以及
None／One／二成员 chain／四成员 chain／在四成员 base chain 上局部 None 的显式
C ABI dispatch。Context 构造不计入 dispatch 时间。Native capture-free handler 打开
经过检查的参数载体并增加逐调用计数，因此时间包含这些 fixture 检查和 base callback，
不是纯 dispatcher 开销。
每项先预热 `min(iterations, 1000)` 次，再测量请求的迭代次数。逐次与总计数会拒绝
漏执行 handler、重复续体或仍执行 base chain 的 None override。CI 的 `10 4` smoke
必须输出 `checksum=210, continuation_calls=100, fragment_calls=140`，不依赖时间判定。
候选查询处于控制平面，允许随 catalog 大小变化；dispatch 是热路径，必须与候选数量
脱钩。已有非计时结构测试继续负责硬性回归判定，微基准仅提供性能证据，不能以不稳定
的固定时间阈值决定 CI 成败。
各平台 CI 仅显式构建探针并执行极小的正确性 smoke，不比较输出时间。
另运行 `11 64` 和 `1001 256`，分别覆盖奇数激活计数与预热上限；预期最终计数为
`232/110/154` 和 `21011/10005/14007`（checksum／continuation／Fragment）。

#### 本机测量快照（2026-09-26）

这是历史固定顺序协议。不能与下方交错协议直接比较绝对时间：fixture 组织与编译出的
探针代码已改变，尽管 Runtime 实现语义没有变化。

环境：Windows 11 build 26200、Intel Core i7-12700（12 核／20 逻辑处理器）、
MSYS2 CLANG64 Clang 20.1.8、C++17、Ninja RelWithDebInfo，开启严格警告。
Runtime 实现：`2c9cf754f17678978dd9f1fa2eb4c50689f001cb`；探针 Git blob：
`d6c94daea117324b30b01b9c8d90b4cabe124200`。
按 4、64、256 行依次运行，每组 5 个独立进程，每项 100000 次测量、1000 次预热。
15 次均核对通过 `checksum=1060500, continuation_calls=505000, fragment_calls=707000`。
未控制 CPU affinity、功耗策略与后台活动。

下表为 ns/op 中位数，括号内为观察到的最小值—最大值：

| 项目 | 4 行 | 64 行 | 256 行 |
| --- | --- | --- | --- |
| candidate_snapshot | 651.6 (637.2–721.5) | 6671.2 (6534.8–6821.2) | 25699.0 (25501.5–26090.7) |
| ref_plus_binding_set | 861.1 (830.7–904.8) | 850.0 (846.1–885.6) | 847.2 (826.1–866.8) |
| refs_plus_chain_4 | 1948.0 (1927.2–1980.6) | 1981.0 (1894.5–2054.9) | 1925.0 (1899.1–2149.2) |
| local_override_none | 279.4 (274.5–295.3) | 281.6 (270.5–295.7) | 273.3 (271.2–275.6) |
| safe_point_activate_and_pin | 56.4 (55.5–56.9) | 54.7 (53.9–56.4) | 54.5 (52.9–55.3) |
| dispatch_none | 217.2 (212.6–233.6) | 213.1 (205.1–222.1) | 210.5 (208.1–225.3) |
| dispatch_one | 503.6 (495.4–523.5) | 720.1 (678.5–748.5) | 493.3 (483.8–930.0) |
| dispatch_chain_2 | 730.7 (707.3–759.8) | 911.3 (876.6–918.4) | 1398.2 (1062.8–1540.1) |
| dispatch_chain_4 | 1186.1 (1145.8–1249.9) | 1362.6 (1347.0–1480.1) | 2147.7 (2051.7–2372.3) |
| dispatch_override_none | 222.9 (212.0–229.6) | 213.6 (210.2–218.6) | 423.2 (416.5–564.5) |

候选发现体现了预期的扫描成本。Dispatch 的结构不访问 catalog，但这些样本**不能**
证明实测延迟恒定：chain 和 override 时间明显变化，原因尚未隔离。没有进一步证据时，
既不能归因于目录扫描，也不能直接当作调度噪声忽略。性能验收仍需受控、交错运行，
以及独立的 Linux／macOS 测量。这是 native fixture 证据，不是编译插件工作负载或发布授权。

#### 交错观察协议

独立模式 `--interleaved [iterations] [rounds]` 默认每项测量 10000 次、30 轮。同进程先
创建独立的 4／64／256 行 fixture 及全部五个 execution context，再开始计时。
每轮测量全部 30 个“目录／用例”组合；从零计数的轮次 `r` 将时间位置 `p` 映射为
`(p + 7*r) % 30`。30 轮内，每个组合恰好占据每个位置一次。轮数为 30 的倍数时位置
平衡；较短运行会明确报告 `position_balanced=no`，不能作为等价证据。

```sh
cmake --build build-perf --config Release --target runtime-fragment-benchmark
cmake -DLUNA_FRAGMENT_BENCHMARK_EXECUTABLE="$PWD/build-perf/runtime-fragment-benchmark" \
  -DLUNA_FRAGMENT_BENCHMARK_ITERATIONS=10000 \
  -DLUNA_FRAGMENT_BENCHMARK_RECORD="$PWD/build-perf/fragment-cost-interleaved.csv" \
  -P tests/runtime_fragment_benchmark.cmake
```

Windows／多配置构建需调整可执行文件路径。脚本省略迭代参数时只运行三次，用于快速
协议检查。它独立核对 900 行顺序、源码 SHA-256、metadata、包含预热的精确计数、非完整
轮次标记及严格 CLI 拒绝；全部通过才写记录。CSV 注释携带构建 HEAD、探针源码摘要、
构建类型、编译器、C++ 方言及采样配置；单独的 HEAD 不证明工作区干净，也不证明完整
Runtime provenance。每个样本保留轮次与位置，供检查时间相关效应。
每个样本仍预热 `min(iterations, 1000)` 次；fixture 创建、CSV 输出、总计检查不在计时内，
native handler／逐调用检查仍在计时内。CPU affinity、功耗策略与后台活动未控制且明确
标注；交错只减少顺序混杂，并非控制全部因素。

Linux C++17／C++23、macOS、Windows CI 均运行 10000 次／30 轮，以 `fragment-cost-*`
保留已验证的 CSV artifact 14 天，不用任何 ns/op 数值决定成败。这是共享 runner 的观察，
不是性能验收。稳定证据归档、受控 affinity／功耗实验、多个独立进程复测及编译插件工作
负载仍属后续发布工作。

本机交错快照：与上方相同的 Windows／Clang 20.1.8 机器，Runtime 实现为
`1663a0b05b16506702ea467cd1cca0c8b8a26e25`，探针源码 SHA-256 为
`2510865763f7e27424d677fcc35c0d8ca429a8db741acbfa4c2dd76bbb6e71f6`。
一个进程，每格 30 个样本，每个样本测量 10000 次、预热 1000 次；900 个样本均通过
顺序与计数核对。下表为 ns/op 中位数（最小值—最大值）：

| 项目 | 4 行 | 64 行 | 256 行 |
| --- | --- | --- | --- |
| candidate_snapshot | 643.55 (609.9–737.5) | 6834.65 (6595.7–7366.3) | 26379.90 (25241.8–29402.8) |
| dispatch_none | 216.15 (202.7–673.7) | 214.15 (201.8–462.2) | 216.10 (202.8–254.8) |
| dispatch_one | 505.95 (472.6–948.5) | 499.45 (477.4–716.3) | 511.80 (472.5–900.9) |
| dispatch_chain_2 | 730.05 (675.2–1014.7) | 718.35 (664.5–1084.7) | 727.95 (684.4–781.7) |
| dispatch_chain_4 | 1223.90 (1137.9–1342.2) | 1200.10 (1077.7–1388.3) | 1206.10 (1085.2–1406.3) |
| dispatch_override_none | 217.15 (202.9–292.6) | 218.10 (201.8–246.6) | 215.70 (202.1–307.4) |

本次未复现旧表中 chain／override 分派随目录规模增长的中位数变化，但仍有离群值，
原因未隔离。由于 fixture 组织、迭代次数及编译出的探针也改变，不能只归因于顺序，
也不能声称 Runtime 获得了某个幅度的优化。

#### 测量线程亲和性

原生探针新增只读 `--affinity-info` 与 `--pinned-thread CPU [iterations] [rounds]`。
CPU 必须是规范的无符号十进制逻辑索引，由调用方从报告的允许集合显式选择，不是物理
核编号，也不提供自动拓扑策略。默认仍为 10000 次／30 轮。固定模式使用独立的
`luna.fragment-cost.pinned-thread.v2`，除探针摘要外，通过 `affinity_control_sha256`
绑定共用的 `benchmarks/fragment_thread_affinity.h` 控制器。旧固定 v1 记录保留为历史观察，
不重写，也不追溯赋予控制器身份。默认交错 v1、未固定编译 v2／bundle／证据格式不变。

```sh
./build-perf/runtime-fragment-benchmark --affinity-info
# 将 0 换成报告的允许 CPU；使用全新的输出路径。
cmake -Werror=dev -DLUNA_FRAGMENT_BENCHMARK_EXECUTABLE="$PWD/build-perf/runtime-fragment-benchmark" \
  -DLUNA_FRAGMENT_BENCHMARK_LOGICAL_CPU=0 \
  -DLUNA_FRAGMENT_BENCHMARK_ITERATIONS=10000 \
  -DLUNA_FRAGMENT_BENCHMARK_RECORD="$PWD/build-perf/fragment-cost-pinned-thread.csv" \
  -P tests/runtime_fragment_benchmark.cmake
```

只约束新探针的测量线程。Windows 使用
[线程亲和性](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask)，
只接受单 processor group（group 0）；Linux 使用
[调用线程亲和性](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html)
及固定 `CPU_SETSIZE` mask。更大的内核 mask、不允许的 CPU、系统调用失败和不支持的平台
都 fail closed，绝不回退为未约束运行。macOS 与多 group Windows 明确报告不支持。
不改变调用方进程，也不改全局功耗、优先级或调度策略。绑定后及每个样本前后，在计时外
核对实际 mask 和当前 CPU；metadata 记为 `affinity=measurement_thread`、
`verified=sample_boundaries` 及请求 CPU／group，功耗仍是 `uncontrolled`。
这不证明外部程序不可能在样本内部改变亲和性。

各平台 CI 独立运行 `tests/runtime_fragment_affinity.cmake` 正确性冒烟：只读查询能力，
在第一个允许 CPU 上做三次迭代、900 个顺序／计数核对，并拒绝不允许／不支持的请求。
自动取第一个 CPU 是冒烟策略，不是性能验收的 CPU 选择策略。artifact 不增加原生固定线程
CSV；下方独立的编译序列保留 14 天。独立进程测量、频率／功耗／后台控制、拓扑记录和长期归档仍需各自证据。
边界检查会影响样本之间的缓存／调度，不能把固定与未固定记录当成完全相同的 harness，
也不能据此声称 Runtime 获得加速。不改变生产 Runtime 或源码语言 API。

#### 编译派发线程亲和性

编译探针共用该控制器，提供只读 `--compiled-fragment-affinity-info` 及显式
`--compiled-fragment-cost-pinned-thread CPU [iterations] [rounds] [O0|O2|O3]`。
默认 10000 次／9 轮／O0。非法／不支持／不允许的 CPU 请求在编译 fixture 前失败；
编译、验证装载、绑定和 320 次正确性核对完成后，再检查允许集合并固定派发测量线程。
探针不固定已有 ORC worker。每个样本前后在计时外核对主线程的 mask 与实际 CPU。

```sh
./build-perf/moonir-canonical-test --compiled-fragment-affinity-info
# 选择一个允许的逻辑 CPU，并使用全新的输出路径。
cmake -Werror=dev -DLUNA_COMPILED_PROBE_EXECUTABLE="$PWD/build-perf/moonir-canonical-test" \
  -DLUNA_COMPILED_PROBE_LOGICAL_CPU=0 -DLUNA_COMPILED_PROBE_PROFILE=O2 \
  -DLUNA_COMPILED_PROBE_ITERATIONS=10000 \
  -DLUNA_COMPILED_PROBE_RECORD="$PWD/build-perf/compiled-pinned-O2.csv" \
  -P tests/compiled_fragment_benchmark.cmake
```

独立原始记录协议 `luna.compiled-fragment-cost.pinned-thread.v1` 新增控制器 SHA-256、
CPU／group／边界核对 metadata，以及
`setup_affinity=uncontrolled,measurement_scope=dispatch_samples`。setup 耗时**不是固定线程
观察值**。新的纯 `compiled_fragment_pinned_protocol.cmake` 先要求这些身份精确匹配，再复用
未改变的 81 样本顺序、调用数／checksum 及构建／工作负载检查。派生校验字符串不替换或
重标注原始字节；固定 reader 与默认 reader 互相拒绝另一模式。
共用亲和性冒烟还做三次迭代的 O0／O2／O3 派发检查（243 样本）、可用时的第二个合法
CPU，以及不支持平台的显式拒绝。CMake 4.4 子调用使用 author 警告参数名称。

已有 v1 series、默认 bundle 入口、证据导出和默认汇总模式仍只接受未固定的编译 v2。固定原始
记录不加入其文件清单。下方独立固定序列不是可迁移证据导出，也不是默认汇总模式的输入；
不要把这些 CSV 放进已有的封闭 bundle。默认 CTest 只增加
合成记录检查，不固定线程，也不设计时阈值。频率、功耗、后台负载、长期存储与性能／发布
批准仍不作保证。

#### 固定线程序列与离线验收

`tests/compiled_fragment_pinned_series.cmake` 要求显式
`LUNA_COMPILED_PROBE_LOGICAL_CPU` 和全新 `LUNA_COMPILED_PINNED_SERIES_OUTPUT_DIR`。
复用六种配置排列、轮次旋转及组内位置／有向相邻检查。每轮 18 个全新测量进程、每配置
六个进程，各有九个位置平衡轮次，共 1458 个样本。测量记录之间不插入能力查询、默认
或非法 CLI 冒烟子进程。支持 Windows／Linux；不支持平台、不允许的 CPU 和进程失败
不回退为未约束观察。全部进程验证后才创建输出目录，manifest 最后写入。

```sh
cmake -Werror=dev -DLUNA_COMPILED_PROBE_EXECUTABLE="$PWD/build-perf/moonir-canonical-test" \
  -DLUNA_COMPILED_PROBE_LOGICAL_CPU=0 -DLUNA_COMPILED_PROBE_ITERATIONS=10000 \
  -DLUNA_COMPILED_SERIES_CYCLES=1 \
  -DLUNA_COMPILED_PINNED_SERIES_OUTPUT_DIR="$PWD/build-perf/compiled-pinned-series" \
  -P tests/compiled_fragment_pinned_series.cmake
cmake -Werror=dev -DLUNA_COMPILED_PINNED_BUNDLE_DIR="$PWD/build-perf/compiled-pinned-series" \
  -DLUNA_COMPILED_PINNED_BUNDLE_EXPECTED_COMMIT=<完整构建提交> \
  -DLUNA_COMPILED_PINNED_BUNDLE_EXPECTED_MANIFEST_SHA256=<manifest字节摘要> \
  -P tests/compiled_fragment_pinned_bundle.cmake
```

选择报告的允许 CPU，调整 Windows／多配置路径，并使用全新输出目录。独立 sampler 省略
迭代参数时默认三次；显式 CI 观察策略用 10000 次、一轮，并在 sampler **外部**选择第一个
允许 CPU。这不是拓扑或性能验收策略。不支持的主机不生成固定目录，也不生成未约束替代数据。

封闭的 `luna.compiled-fragment-pinned-series.v1` 目录只包含 `manifest.csv`、`samples.csv`
及 `18 * cycles` 份原始记录（1／2／10 轮分别为 20／38／182 个文件）。绑定未改变的共享
validator、固定 validator、sampler 和亲和性控制器的源码摘要，声明 CPU／group 与未固定
setup 范围，保留原始／合并行的精确映射、分配置 materialization key、共同构建／工作负载
身份及文件字节摘要。固定 reader 与默认 reader 共用私有校验核心，但由可信显式入口选择
模式，不按 manifest 或环境标志自动切换；互相拒绝另一协议。reader 只读，并在返回前再次
核对全部原始／合并字节；不启动探针、不执行归档代码、不推导胜者，也不批准延迟或发布。
可选提交／manifest 锚点只检测不匹配，本身不证明真实性。

CI 将固定序列导出为下方 `compiled-fragment-pinned-evidence/`，放在
`compiled-fragment-evidence/` 的**同级**，而非内部，artifact 名称和
14 天保留策略不变。macOS 或不支持的 Windows group 明确省略该目录。默认合成门禁覆盖
LF／CRLF、多轮、输入字节不变、缺失／未列出／符号链接／不安全路径、metadata／源码／
顺序损坏、重新摘要的非法记录、模式隔离和失败不发布；固定记录／bundle 门禁共用合成
fixture。此目录仍需要匹配的可信 checkout 字节：携带摘要，不携带可迁移的控制器／
validator 源码快照；下方独立证据包补齐这些字节。功耗／后台控制实验及长期归档
仍需另行完成；setup 及组间顺序平衡仍不作保证。

#### 编译插件对比协议

`moonir-canonical-test --compiled-fragment-cost [iterations] [rounds] [O0|O2|O3]` 复用编译器
harness 的 frontend/backend 链接，不使用 native 替身 handler。
`benchmarks/compiled_fragment/` 包含独立的 host/plugin 库包。探针以 MoonIR O2 编译两包，
编码并自校验真实 Moon Container，从独立验证的 owner 解码获得公开证据，然后通过
`loadVerifiedMoonGenerationOnce` 加载；不直接注册 JIT 绕过验证。适配器默认仍为 LLVM IR
O0，现可显式选择 O2／O3。v2 协议记录确切 IR 级别及 `orc_codegen=default`，ORC 机器码
生成设置不变。MoonIR O2 本身不会选择 LLVM IR O2；生产默认值与 Runtime C ABI 不变。

每次输入为 `call_index % 1024`；令 `B = input * 3`：

| 计时路径 | 预期结果 | 入口 ABI |
| --- | --- | --- |
| plain、private_erased、static_resume | B + 17 | i32 → i32，无 context |
| static_discard | B | i32 → i32，无 context |
| dynamic_none、dynamic_one、dynamic_chain_2、dynamic_chain_4、dynamic_override_none | B + 17 | 显式 context + i32 → i32 |

宿主显式按 `resume_a/b/c/d` 顺序选择候选。生成的 Copy factory 收到 `mask=0`，因此
全部计时 handler 都 resume；绑定链长度核对为 1/2/4。局部 None 覆盖 chain-4，且不改变
父绑定。每个配置执行 320 次非计时输出检查：32 个输入分别经过九种路径，另加
32 次 factory `mask=1` 检查以区分 resume/discard。默认 CTest 覆盖三种配置，共 960 次
结果检查，另加配置／缓存拒绝门禁，不调用计时模式。Runtime 在全部结果检查与采样前销毁，
pinned entry 和 context 保留生成的代码与环境；这不新增 Luna 线程或 native code
隔离保证。

计时必须显式启用，默认 10000 次迭代、九轮；允许范围为迭代 1..10000000、轮数 1..90。
每轮测量全部九种路径，采用 `(position + 4*round) % 9`（从零开始）。九的倍数为位置
平衡轮数；不足整组明确标为 `no`。每个样本预热 `min(iterations,1000)` 次，测量时重置
输入序列，计时结束后核对闭式 checksum。CSV 的 `calls` 和 `checksum` 包含预热；
`ns_per_op` 仅覆盖正式测量。checksum 验证最终输出，**不证明**各层 handler 的事件次数。
native harness 的 case 分支、输入变化、间接 JIT 调用和 checksum 累加均在计时内。
基线采用不同的无 context ABI；差值是端到端观察，不是孤立 Slot 指令成本。

compile/encode/owner-decode、verified load/JIT、候选发现/factory/绑定准备、结果验证
和输出都在 dispatch 计时之外。三项 setup 时长只是分别一次观察，不是统计采样的 setup
benchmark。metadata 包含 build HEAD、探针 SHA-256、六个工作负载文件的 path/hash 聚合、
实际 host/plugin 容器 digest、构建类型、native 编译器、C++ 方言、LLVM 版本、target/layout、
优化配置、materialization key 及未受控 affinity/功耗。HEAD 本身不证明工作树干净或完整构建来源；产物 digest
可因 target 和源码 location 不同而变化。

```sh
cmake --build build --target moonir-canonical-test --parallel
cmake -Werror=dev -DLUNA_COMPILED_PROBE_EXECUTABLE="$PWD/build/moonir-canonical-test" \
  -DLUNA_COMPILED_PROBE_ITERATIONS=10000 -DLUNA_COMPILED_PROBE_PROFILE=O2 \
  -DLUNA_COMPILED_PROBE_RECORD="$PWD/build/compiled-fragment-cost-O2.csv" \
  -P tests/compiled_fragment_benchmark.cmake
```

Windows／多配置构建需调整可执行路径。脚本默认三次迭代用于协议检查，独立核对 81 个
样本、case/position 平衡、精确 calls/checksum、源码/工作负载 hash、metadata、不完整
轮次标记及非法 CLI 拒绝，全部验证后才写 CSV。Linux C++17/C++23、macOS 和 Windows
CI 先运行三次迭代的 CLI 预检，再运行下面的多进程系列。ns/op 和 setup 时长都不作为
成败阈值。共享 runner 样本不关闭 SF008：受控机器状态、更广的复测及稳定证据归档
仍属于性能验收工作。
脚本默认配置仍是 O0，O2／O3 必须通过 `LUNA_COMPILED_PROBE_PROFILE` 显式选择。

编译器 adapter 在已验证 lowering 前设置 `CodeGenerator` 的 LLVM IR 级别。可选的
generation `materializationKey` 将产物摘要与生成代码配置分开，包含 LLVM 版本、IR
级别与默认 ORC 策略。adapter 的早期缓存检查与 Runtime 的加锁 load-once 路径都拒绝
不同 key，保持 active history 和输出句柄不变。同配置复用前仍验证 target/layout、
完整性及 owner Slot 证据。显式 activation／rollback 可切换配置，旧 pin 仍保留原代码。
这是兼容的 C++ 源级控制面扩展，不改变容器／sysmeta schema 或 Runtime C ABI。
key 是可信 loader 的声明，不是加密证明或 native code 沙箱。

#### 多进程重复对比协议

`tests/compiled_fragment_series.cmake` 为每份测量记录启动全新二进制进程，不在系列中
穿插默认／非法 CLI 预检进程。一轮包含 O0／O2／O3 的全部六种排列：六组各三个进程，
共 18 个进程、每配置六个独立进程、1458 个 dispatch 样本。每配置在每个组内位置出现
两次，每对不同配置的有向相邻关系在**组内**出现两次；不声称组间过渡也平衡。
额外轮次轮转排列组的顺序。轮数允许 1..10，默认 1；迭代默认三次用于 smoke，允许
无前导零的十进制 1..10000000，CI 使用 10000。

每进程复用 v2 校验器，检查 81 个样本、计数、checksum、构建与产物事实。整个系列
必须保持 host/plugin 容器摘要、target/layout、构建／来源身份、采样配置与 ORC 策略
一致；materialization key 分配置保持一致。必需 metadata key 恰好出现一次，SHA-256
不接受附加后缀。这验证独立进程复测，而不是同一进程重复使用缓存 generation。

全部结果在内存校验后才写 bundle。输出目录必须尚不存在，不覆盖或删除既有路径。
bundle 包含 `process-N-PROFILE.csv`、前置 process／cycle／block／组内 position／profile
列的 `samples.csv`，以及最后写出的 `manifest.csv`。manifest 保存共同来源、采样期间
核对未变化的 runner／validator 源码摘要、各配置 key、
计数／顺序保证、每份原始文件和合并 CSV 的实际字节 SHA-256。写后读回并按 LF／CRLF
规范化核对内容，再发布 manifest。写入中断可能留下没有完整 manifest 的部分目录；
消费者必须验证列出的摘要，不能把目录存在视为成功。摘要证明完整性，不证明真实性、
干净工作树或可复现性。

```sh
cmake -Werror=dev -DLUNA_COMPILED_PROBE_EXECUTABLE="$PWD/build/moonir-canonical-test" \
  -DLUNA_COMPILED_PROBE_ITERATIONS=10000 -DLUNA_COMPILED_SERIES_CYCLES=1 \
  -DLUNA_COMPILED_SERIES_OUTPUT_DIR="$PWD/build/compiled-fragment-series" \
  -P tests/compiled_fragment_series.cmake
```

Linux C++17/C++23、macOS 与 Windows CI 将完整 bundle 和 native 探针一起保留 14 天。
默认合成 CTest 覆盖 LF／CRLF、轮数 1／2／6／10，拒绝重复 metadata、损坏计数／checksum、
header／digest、变化的来源、重复／截断调度及既有输出目录，不启动计时探针。这不增加
语言、容器或 Runtime API。进程／组调度、affinity／功耗及共享 runner 负载仍未受控，
该协议不关闭性能验收。

#### 离线 bundle 验收

`tests/compiled_fragment_bundle.cmake` 对已解包的 v1 bundle 做只读验收，不启动探针、
不解压归档、不写文件、不设置计时阈值。目录必须恰好包含 manifest、合并 CSV 及列出
的原始记录；缺失、未列出及符号链接项会被拒绝。记录名必须是规范的
`process-N-PROFILE.csv` basename，不允许路径穿越。manifest／每份原始记录上限
64 KiB，合并 CSV 上限 4 MiB，轮数仍不超过十。必需 manifest key 唯一，拒绝未知 key／
文本；声明的计数、顺序和平衡必须符合 v1 调度。

检查器验证实际文件字节 SHA-256、每份 v2 原始记录、共同来源及分配置 key，然后从
原始记录重建合并 CSV，逐行核对映射。错误 checksum、变化的来源或错误合并映射，
即使重新计算了相关摘要也会被拒绝。runner／validator 摘要必须与源码 checkout 的
实际字节匹配，共用校验器也检查原始记录的探针／工作负载摘要。归档时保留匹配的源码
checkout；checkout 换行方式变化也可能改变字节摘要。这不是任意历史版本读取器。

```sh
cmake -Werror=dev -DLUNA_COMPILED_BUNDLE_DIR="/path/to/compiled-fragment-series" \
  -DLUNA_COMPILED_BUNDLE_EXPECTED_COMMIT="<完整小写40位十六进制提交>" \
  -DLUNA_COMPILED_BUNDLE_EXPECTED_MANIFEST_SHA256="<小写64位十六进制manifest摘要>" \
  -P tests/compiled_fragment_bundle.cmake
```

两个预期锚点均可选。若指定，应取自独立可信的归档／CI 事实；从同一不可信 bundle
计算锚点不能证明真实性。提交 metadata 本身是自报信息，不证明工作树干净、完整构建
来源或可复现性。验收期间输入必须保持不变：读取前后摘要检查可以检测变化，但不提供
文件系统快照或针对并发恶意修改的沙箱。未提供锚点时，这只是匹配源码字节的自洽性
验收，不是来源认证、性能验收或发布授权。

四个跨平台／方言观测作业在上传前使用 workflow 提交作为预期锚点运行检查器。默认
CTest 新增合成文件夹具的验收／拒绝（包括重新摘要的损坏数据）、只读字节检查、LF／
CRLF 及 1／2／10 轮；Unix 还检查符号链接记录。夹具仅保留在构建目录内的唯一目录，
不覆盖观测结果，不执行计时代码。CI 的 14 天保留策略未改变；长期证据存储与受控
机器的性能测量仍是独立未完成项。

#### 可迁移证据导出

`tools/package_compiled_fragment_evidence.cmake` 将已验收 bundle 导出到全新目录，其
父目录必须已存在且不是符号链接。必须指定预期观测提交及原始 manifest 的实际字节
SHA-256。拒绝既有输出路径，以及嵌套在输入 bundle 内的输出。不覆盖／删除观测，
不创建 Release、不上传外部存储、不改变保留策略。

证据包包含原始 `bundle/`、`source/` 下十二份原字节文件（reader、protocol、runner、
打包／字节检查工具、探针 C++ 及六份工作负载输入），以及最后写出的 `evidence.csv`。
v1 索引规定封闭且排序的路径／摘要清单，保存观测提交／manifest 锚点、轮数、
`source_snapshot=validation-inputs-only` 和 `approval=none`。其中 `bundle_git_commit`
仅指观测的提交；配套校验工具按字节摘要标识，不宣称属于该历史提交。复制保留字节，
包括源码／记录换行；发布索引前再次核对输入／副本摘要，然后校验完整证据包。中断可能
留下部分目录，目录／索引存在本身不代表验收成功；不要复用部分目录作为输出。

```sh
cmake -Werror=dev -DLUNA_COMPILED_EVIDENCE_BUNDLE_DIR="/path/to/compiled-fragment-series" \
  -DLUNA_COMPILED_EVIDENCE_OUTPUT_DIR="/existing/parent/new-evidence-directory" \
  -DLUNA_COMPILED_EVIDENCE_EXPECTED_COMMIT="<观测提交>" \
  -DLUNA_COMPILED_EVIDENCE_EXPECTED_MANIFEST_SHA256="<manifest字节摘要>" \
  -P tools/package_compiled_fragment_evidence.cmake
```

导出时输出 `evidence.csv` 的实际字节 SHA-256。应在证据包之外独立保存此索引锚点及
可信 CI／源码事实。使用可信 checkout 中的 `tools/compiled_fragment_evidence.cmake`
只读检查已解包证据包的完整封闭目录树及每份文件摘要：

```sh
cmake -Werror=dev -DLUNA_COMPILED_EVIDENCE_DIR="/path/to/copied-evidence-directory" \
  -DLUNA_COMPILED_EVIDENCE_EXPECTED_COMMIT="<观测提交>" \
  -DLUNA_COMPILED_EVIDENCE_EXPECTED_INDEX_SHA256="<独立可信索引摘要>" \
  -P tools/compiled_fragment_evidence.cmake
```

字节检查不会执行／include 包内脚本，也**不重复** bundle 协议验收：重新计算摘要的
脚本仍只是数据，同时修改文件及自报索引不能证明真实性。可选的预期索引／提交锚点
必须来自独立可信事实。接收方确认校验源码字节可信后，才可以手工使用包内
`source/tests/compiled_fragment_bundle.cmake` 检查 `bundle/`，并指定原始预期提交／
manifest 锚点；迁移后不再依赖原始源码路径或 Git checkout。不可仅凭不可信索引或
未固定锚点的字节检查成功就执行包内代码。

这些源码仅覆盖配套 reader 的校验输入，不是完整构建／可复现性快照、编译器／JIT
二进制、native 探针记录、attestation 或签名；若需要 native CSV，应另行保存同级
记录。检查／导出期间输入必须不变，不提供文件系统快照或抵御并发恶意修改的沙箱。
证据包没有计时阈值、性能／发布批准，不改变 SF008 范围。

Linux C++17／C++23、macOS、Windows CI 在 bundle 验收后导出证据包，并将
`compiled-fragment-evidence/` 与 native CSV 一起上传，artifact 名称及 14 天保留
策略不变。默认合成 CTest 覆盖 LF／CRLF、1／2／10 轮、迁移、观测不变、包内脚本
不执行、既有／嵌套输出、损坏／缺失／不安全清单；Unix 还检查符号链接记录，不启动
计时程序。该包只是便于迁移到另行选定的存储；永久后端、保留／访问策略及受控性能
验收仍未完成。正式 release evidence／attestation 工作流及生态锁不变。

#### 固定模式可迁移证据

`tools/package_compiled_fragment_pinned_evidence.cmake` 从显式入口导出已验收的固定
bundle；`tools/compiled_fragment_pinned_evidence.cmake` 提供对应的只读字节检查入口。
协议分别为 `luna.compiled-fragment-pinned-evidence.v1` 与
`luna.compiled-fragment-pinned-series.v1`，默认入口互相拒绝另一模式，不按 metadata 自动切换。
两种模式复用私有导出／检查核心；旧证据 v1 的 12 份源码清单及索引格式不变。

固定包包含 `bundle/` 的原始字节、`source/` 的 17 份固定校验输入和最后发布的
`evidence.csv`。清单将默认 sampler 换为固定 sampler，并增加固定 bundle reader、固定
protocol validator、`fragment_thread_affinity.h`、固定证据 reader 及固定 exporter。
保留共享 reader／validator、导出／检查核心、探针 C++ 和六份工作负载输入。
索引保存观测提交／manifest 锚点、轮数、排序的路径／摘要、
`source_snapshot=validation-inputs-only` 与 `approval=none`；CPU／group／scope／控制器身份
仍保留在包内原始 manifest／记录中。1／2／10 轮分别为 38／56／200 个文件（包含索引）。

```sh
cmake -Werror=dev -DLUNA_COMPILED_PINNED_EVIDENCE_BUNDLE_DIR="/path/to/pinned-series" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_OUTPUT_DIR="/existing/parent/new-pinned-evidence" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_EXPECTED_COMMIT="<观测提交>" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_EXPECTED_MANIFEST_SHA256="<manifest字节摘要>" \
  -P tools/package_compiled_fragment_pinned_evidence.cmake
cmake -Werror=dev -DLUNA_COMPILED_PINNED_EVIDENCE_DIR="/path/to/copied-pinned-evidence" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_EXPECTED_COMMIT="<观测提交>" \
  -DLUNA_COMPILED_PINNED_EVIDENCE_EXPECTED_INDEX_SHA256="<独立可信索引摘要>" \
  -P tools/compiled_fragment_pinned_evidence.cmake
```

导出要求提交与 manifest 锚点、全新输出及既有非符号链接父目录；不覆盖观测，不允许输出
嵌套在输入中，预检失败不创建输出。复制前后核对字节及完整协议，索引最后写入；写入中断
仍可能留下部分目录。两种字节 reader 在返回前再次检查清单内文件摘要，不执行包内脚本，
也不重放完整 bundle 协议。重新摘要的包内脚本只作为数据；必须先从独立可信事实确认
源码字节，才可手动运行包内 `source/tests/compiled_fragment_pinned_bundle.cmake`，指定
`LUNA_COMPILED_PINNED_BUNDLE_DIR` 与原始提交／manifest 锚点。迁移后的配套 reader
不需要原始 checkout。Python 汇总仍使用当前可信 checkout，不自动执行包内源码。

CI 将该包作为 `compiled-fragment-evidence/` 的同级上传，原始固定 bundle 位于包内，
不再重复上传松散目录；不支持的平台不生成固定包，artifact 名称／14 天策略不变。
共用合成门禁覆盖源清单数量、迁移、输入／输出只读、模式互斥、控制器缺失／损坏和预检失败
不发布，不启动计时或亲和性代码。源码快照不是完整构建、探针二进制、签名或 attestation；
摘要不证明真实性或可复现性，不抵御恶意并发文件修改。性能批准、功耗／后台控制及长期
存储仍未完成，不改变 Runtime ABI、SF008 范围或正式 release evidence 工作流。

#### 离线描述性汇总

`tools/summarize_compiled_fragment_bundle.py` 只需 Python 3.8+ 标准库，先通过**当前可信
checkout** 的 CMake reader 验收解包后的 bundle，再向 stdout 输出
`luna.compiled-fragment-summary.v1` JSON。不执行探针或包内脚本、不解压归档、不写入
输入／输出文件。`--cmake` 指定可信程序，默认使用 PATH 中的 `cmake`。协议、探针、
工作负载及 runner 源码字节必须匹配观测身份，与普通离线验收相同；不是任意历史读取器。

```sh
python3 tools/summarize_compiled_fragment_bundle.py \
  --bundle "/path/to/compiled-fragment-series" \
  --expected-commit "<完整小写观测提交>" \
  --expected-manifest-sha256 "<独立可信manifest字节摘要>"
```

默认模式也可显式写为 `--mode uncontrolled`，reader 和报告协议保持不变。只有显式
`--mode pinned` 才选择固定 reader，输出独立的 `luna.compiled-fragment-pinned-summary.v1`。
不根据 metadata 自动识别或回退；模式与 bundle 不匹配时拒绝且不输出报告。两种模式
复用小数／进程聚合逻辑。固定报告保留全部 manifest metadata，包括 CPU／group、控制器及
固定 validator 摘要、分配置 materialization key 和未固定 setup 范围：

```sh
python3 tools/summarize_compiled_fragment_bundle.py --mode pinned \
  --bundle "/path/to/compiled-fragment-pinned-series" \
  --expected-commit "<完整小写观测提交>" \
  --expected-manifest-sha256 "<独立可信manifest字节摘要>"
```

亲和性只约束 dispatch 测量线程，并在样本边界验证，不约束 setup、功耗、后台负载或
进程间调度。边界检查和 harness 成本使固定／默认记录不能视为等价；工具不合并 bundle，
也不比较两种模式。

可选锚点与 reader 的信任边界相同。没有外部 manifest 锚点时，工具固定起始读取的
manifest 字节，只用于内部一致性，不证明真实性。输出完整报告前重新核对原始文件、
合并文件、manifest 的字节摘要及封闭目录清单。校验、程序缺失或解析失败时，stderr
报告错误、返回非零状态，stdout 没有报告。输入必须保持不变；不提供抵御恶意并发
修改的文件系统快照。归档脚本始终只作为数据。

每个进程／路径报告九轮样本的数量、最小值、中位数和最大值；随后按配置／路径汇总
**独立进程的中位数**，而非混合所有轮次：每配置每轮系列六个进程。偶数个值的中位数
取两个中心值的平均，精确小数字符串避免浮点舍入。逐进程保留 cycle、block、组内
profile position 和记录名，报告保留 manifest 身份及汇总工具的实际字节摘要。
setup 阶段单独汇总每个进程的一次观察，仍不是统计采样的 setup benchmark。

报告明确保留 `approval=none`、未受控机器条件、不同入口 ABI 和计时 harness 成本。
不提供置信区间、比值／胜者选择、计时门禁、孤立 Slot 指令成本结论或性能／发布批准。
报告是派生数据，不属于封闭的 v1 bundle／evidence 清单；保存时必须放在这些目录
**之外**。包格式、CI 的 14 天保留策略、Runtime 语义及 SF008 范围均不变。长期存储
及受控性能验收仍保持开放。

默认 `luna.compiled-fragment-summary` 与 `luna.compiled-fragment-pinned-summary` CTest
共用夹具／统计测试，复用 `BUILD_TESTING` 已要求的 Python 3.8+，
不增加仅编译器构建的依赖。合成 LF／CRLF 夹具覆盖零值、精确小数／偶数中位数、1／2 轮
聚合、可区分进程中位数与混合轮次的偏斜分布、输入字节不变、错误锚点、损坏记录及
CMake 缺失。仅执行可信 reader，不启动计时程序。
另检查默认／显式未固定报告一致、模式互斥、非法模式、固定身份保留及亲和性范围声明、
CPU／group／setup 错配，以及重新计算原始摘要后的控制器／checksum 损坏。所有拒绝路径
均无 stdout 报告，输入字节保持不变。

#### 编译插件本机观察性评估（2026-09-27）

本轮只评估已有实现，不修改 Runtime、ABI、探针、计时协议或系统电源／优先级策略。
观察提交为 `303c6bee7bfd9b0232be61fc2625c66dca4ccace`；采样前工作树干净。
Windows 11 build 26200、i7-12700（12 核／20 逻辑处理器）、MSYS2 CLANG64
Clang／LLVM 20.1.8、C++17、Ninja RelWithDebInfo（原生 `-O2 -g -DNDEBUG`、严格警告）。
两批仅固定 dispatch 测量线程在 group 0／逻辑 CPU 0，并在样本边界核对；不宣称它是
某类物理核。开始和结束只读查询均报告 Windows 平衡电源方案，但频率、温度、后台负载、
setup 亲和性与进程间调度未控制／未连续监测。没有并行构建、回归测试或第二个测量批次。

分别先后运行 10000 与 100000 次迭代，每批三轮完整系列：54 个独立进程、每配置
18 个进程、每进程每路径九个样本，共 4374 个样本；两批合计 108 进程／8748 样本。
预热均为 1000 次，MoonIR O2、LLVM IR O0／O2／O3、ORC 默认。全部顺序、来源、调用数、
checksum、原始／合并映射与文件字节校验通过；两批共同 manifest 身份相同，仅迭代配置
和合并文件摘要不同。每批单独汇总，不与上轮一轮系列或默认未固定记录混合。

每个样本是整批调用的平均 ns/op，不是一次调用的延迟。下表为 100000 次批次的
**进程中位数的中位数（进程中位数最小—最大）**，不是 P50／P99 或置信区间：

| 路径 | LLVM O0 | LLVM O2 | LLVM O3 |
| --- | --- | --- | --- |
| plain | 4.50 (4.40–4.50) | 2.00 (1.90–2.00) | 2.00 (2.00–2.10) |
| private_erased | 4.50 (4.40–4.50) | 2.00 (2.00–2.00) | 2.00 (2.00–2.00) |
| static_resume | 2.20 (2.20–2.30) | 2.00 (2.00–2.10) | 2.00 (2.00–2.00) |
| static_discard | 2.00 (2.00–2.00) | 2.00 (2.00–2.00) | 2.00 (2.00–2.00) |
| dynamic_none | 216.30 (209.10–225.40) | 217.05 (208.40–230.20) | 215.30 (211.30–229.50) |
| dynamic_one | 561.35 (544.40–609.80) | 555.15 (542.30–594.00) | 548.90 (531.70–575.70) |
| dynamic_chain_2 | 799.10 (771.60–824.50) | 790.60 (770.30–842.00) | 793.15 (772.30–831.60) |
| dynamic_chain_4 | 1422.75 (1384.20–1535.10) | 1356.15 (1317.60–1401.90) | 1342.85 (1290.50–1440.40) |
| dynamic_override_none | 218.10 (211.20–226.90) | 217.45 (213.00–233.10) | 215.55 (212.30–229.50) |

O2 两批的独立进程中位数如下；不同迭代批次仍是两次独立、先后进行的观察，不能把变化
归因于迭代次数，也不据此选定优化 profile 或宣称加速：

| 路径 | 10000 次 | 100000 次 |
| --- | --- | --- |
| dynamic_none | 209.35 (203.70–221.20) | 217.05 (208.40–230.20) |
| dynamic_one | 546.10 (523.20–581.80) | 555.15 (542.30–594.00) |
| dynamic_chain_2 | 777.75 (751.80–824.40) | 790.60 (770.30–842.00) |
| dynamic_chain_4 | 1339.35 (1285.40–1388.40) | 1356.15 (1317.60–1401.90) |
| dynamic_override_none | 212.50 (206.60–246.10) | 217.45 (213.00–233.10) |

长批次中，O2 One 的三轮系列中位数为 544.45／568.25／562.90 ns/op，四成员链为
1344.70／1369.40／1353.25；分轮未表现为恒定值。O2 None 的单个批次样本平均值范围为
201.50–614.70 ns/op，不能把进程中位数表当成延迟尾部上界。原因未隔离。
O2 长批次的 setup 阶段中位数分别为 11.518 ms（编译／编码／解码）、24.984 ms（验证加载／
JIT）、0.0315 ms（查找、四候选发现、宿主排序、factory、绑定和 context 创建）。
这些各进程一次的 setup 观察没有固定线程，也不是通用反射 benchmark 或大目录证据。

源码检查与时间观察分开解释：`findBindingEntry` 在已固定 BindingSet 上二分查找，不扫描
候选 catalog；`luna_runtime_fragment_dispatch_v1` 每次构造 owning Slot／Contract／参数布局
字符串；`dispatchRuntimeFragmentChain` 每个 handler 调用公开 activation 构造器，后者
使用 `std::make_unique<RuntimeFragmentActivationState>` 并复制身份／参数载体。
这是代码中的分配路径，不是本轮测得的分配次数或耗时归因。None 的来源检查、快照 pin、
回调和 capture 回写，以及不同入口 ABI／harness 成本都在综合路径中；不以 plain 差值
宣称纯 Slot 指令开销，static_discard 的计算结果也不同。

下一轮推荐先验证**同步链路内部 activation 的作用域内存方案**，保持公开拥有型
activation API、名义／布局验证、single-shot、失败传播、逃逸／cleanup、嵌套调用及
generation pin 不变，关注深链栈占用；先做非计时正确性与分配路径检查，再用匹配协议
复测。入口 owning 字符串的优化另拆，必须先明确 C ABI 字节寿命，不能直接改成悬空 view。
这不需要新增候选集合机制，也不扩大 context／重入／non-Copy ABI 范围。
本轮没有实现这些优化，没有设阈值，`approval=none`，受控性能验收仍开放。

本机证据保存在 `build-audit-clang64/fragment-evaluation-303c6be-i10000-c3-evidence/`
与 `fragment-evaluation-303c6be-i100000-c3-evidence/`，每包 74 文件，原始 bundle 位于 `bundle/`。
它们在忽略的构建目录中，不是持久外部存储，也没有自动上传为 CI artifact。
manifest SHA-256 分别为：

```text
10000:  e7bee2874fac69d3f38e73312304d8d7b6229fa70170b5244ffee6084a624f19
100000: 9c92401ecebfedc5d34c15d77254a5f0e26a1d418327981d111d1ee80e4ed1f9
```

证据索引 SHA-256 分别为：

```text
10000:  d5e18925b4b4958630af61ed7695477afa2a2379704a566557ebd5b281a9d437
100000: 0c40d250ad89548d79604af47bfe924e68b718a092732a931d6695fa63823226
```

汇总工具 SHA-256 为 `e35ce22f899950fc79915de09af05d9f4f2eb952a0d082211bc83ff0ca4eeb59`；
实际探针二进制 SHA-256 为 `c2a14bb67460a16f5aeff9afd57e4e9c63972ca02a50f7c6344deee0a21ad682`。
RuntimeFragment.cpp 最后修改提交为 `0c92301928b51018847102687c7b354f499bb7b9`，当前字节
SHA-256 为 `fc7bdaa9bba31506418a0a25f01d09c01f00355c6e7edbc5f6f1ba07248604e1`。
这些身份／锚点用于复核本轮观察，不构成完整构建 attestation 或可复现性证明。

重跑时使用全新路径，选择允许 CPU，分别给固定 series 指定 10000／100000 次与三轮；
随后按固定证据导出和 `--mode pinned` 汇总命令使用对应的提交／摘要。重跑数值不要求
相同；后续报告提交不应被误认成这两批观察的构建提交。

#### 同步链路 activation 作用域存储（2026-09-27）

已实现上述第一项优化：内部链派发在每个同步 handler 的调用帧上创建 activation，
引用外层 dispatch 自己拥有的 Slot／Contract 与参数载体，不再调用公开构造器做
每 handler 的堆分配及身份字符串复制。公开 `RuntimeFragmentActivation` 的拥有型
构造、move 与 opaque API 不变，C ABI 函数／descriptor 布局和 v1 版本不变。
同一私有 state 类型保留拥有型字段，内部帧让它们为空，以避免增加第二套 opaque 表示。

公开与内部路径共用完整 activation 合法性检查；single-shot、sticky failure、
下游诊断、continuation escape、异常传播与完整 BindingSet／generation pin 均保留。
handler token 只在同步 execute（含嵌套 resume）期间有效，不能保存到返回之后使用；
这与旧实现返回时释放 activation 的寿命一致。载体 payload 的边界／寿命仍由宿主负责。
入口字符串构造和每次 dispatch 的拥有型记录快照未改，不能称整个派发零分配。

既有 `luna.runtime-fragment-v1` 新增长身份（避免 SSO 遮蔽复制）、1／4／64 成员链的
普通 C++ 分配计数比较，旧实现会因分配数随链长增长而失败；优化后分配数相同。
计数窗口不含 fixture 创建，handler／base 不自行分配，不覆盖 aligned／系统分配，
不是计时阈值。还覆盖 64+64 个同时存活 activation 的地址独立性、resume 前后参数
匹配／错误身份拒绝、嵌套完成／escape、内层 handler 抛异常后恢复，以及公开对象在
调用者身份／载体修改及销毁后经两次 move 仍保持自己的记录。

ASan／UBSan 配置为此测试直接编译相关 Runtime 实现，不只 instrument fixture，
且不污染安装的 AOT runtime archive。标准严格警告构建与两个 Runtime 测试、
ASan／UBSan 下相同两测试均已通过。深链仍使用递归，测试覆盖不是任意深度的栈界限；
完整非硬件 CTest 77／77 通过（268.39 秒），证据／汇总门禁也保持通过。
本轮不新增链长政策或异步语义。匹配协议的性能复测与受控验收仍待完成，
不把分配路径减少等同于已测得的速度提升。

#### 分配计数夹具 Linux sanitizer 修复（2026-09-27）

复测前检查提交 `6da36eb` 的 CI：macOS／Windows 通过，Linux 的普通 C++17／C++23
与 TSan 通过，但 ASan／UBSan 的 `luna.runtime-fragment-v1` 失败。失败日志为
`alloc-dealloc-mismatch (operator new vs free)`，发生在 libstdc++ 的 `stable_sort`
临时缓冲释放，而非同步 activation 的生命周期检查。

测试计数器替换了普通 throwing `new/delete`，却遗漏 `nothrow` 入口；临时缓冲经
sanitizer 默认 `nothrow new` 分配，随后被测试的 `free` 型 sized delete 释放。
现补齐标量／数组的 nothrow 分配和 cleanup delete，使普通分配家族使用同一分配器，
并在测试中显式检查四次 nothrow 分配计数及普通／cleanup 释放配对。不替换 aligned
分配，不禁用 mismatch 检查，不移除 sanitizer，不修改生产 Runtime／ABI／计时协议。

本机现有 Arch Linux WSL（Clang 22.1.8、libstdc++ 16）以 C++17、O0、
`-fsized-deallocation -fsanitize=address,undefined` 直接编译测试与相关 Runtime，
先复现相同的失败栈，再验证修复后通过；C++23 下同样通过。这不是 Ubuntu CI 环境
复刻或完整 Linux 编译器套件验收。Windows 严格警告普通／ASan／UBSan 配置下的两个
Runtime CTest 均通过。本轮原定匹配协议的性能采样尚未开始，等待修复的远端回归确认。

#### 作用域 activation 的匹配协议复测（2026-09-27）

观察构建提交为 `108193719213ca0344541b736acbc835b54adfe6`，其中生产 Runtime 的
最后修改提交仍是 `6da36eb`；后续修复只改测试计数器。探针按新提交重建，非计时结果
检查通过；采样前工作树干净，采样中未改源码／协议。本轮未实现第二项优化。
报告提交前确认该构建的 Linux／macOS／Windows CI 全部成功，Linux ASan／UBSan
失败已修复；对应 run 为 `36324347602`／`36324347627`／`36324347594`。

仍是 Windows 11 build 26200、i7-12700（12 核／20 逻辑处理器）、Clang／LLVM 20.1.8、
C++17、严格警告 RelWithDebInfo 原生 `-O2 -g -DNDEBUG`，只固定 dispatch 测量线程在
group 0／逻辑 CPU 0。允许 CPU 为 0–19；不假定物理核类别。开始／结束的只读查询
均报告平衡电源方案，频率、温度、后台负载、setup 亲和性与进程调度仍不受控。
此前 Linux 夹具诊断曾使用 WSL，本轮没有关闭／重整后台环境；本代理不在采样期间
并行运行本机构建、回归、WSL 编译或另一批测量。远端 CI 与本机观察独立跟踪。

按原顺序先后运行 10000／100000 次、各三轮系列、1000 次预热、每路径九个批次样本；
MoonIR O2、LLVM IR O0／O2／O3 与默认 ORC 不变。各 54 个新进程、18 个／配置、
4374 个样本，两批合计 108／8748。顺序／身份／调用数／checksum／原始合并映射与
字节校验全部通过。对应旧版 `303c6be` 的共同 manifest metadata 只有提交与合并
文件摘要不同；工作负载、probe／runner／validator／亲和性控制器源码摘要、容器摘要、
配置、materialization key 与计时 harness 相同。两份新 manifest 间只有迭代与合并摘要
不同。实际探针二进制和 Runtime 源码不同，不能由 metadata 相同推导完整构建相同。

以下仍是**进程中位数的中位数（进程中位数最小—最大）**，单位 ns/op；每个基础样本
是整批调用的平均，不是一次调用的延迟。100000 次的新批次完整结果：

| 路径 | LLVM O0 | LLVM O2 | LLVM O3 |
| --- | --- | --- | --- |
| plain | 4.40 (4.30–4.60) | 2.00 (2.00–2.00) | 2.00 (2.00–2.00) |
| private_erased | 4.40 (4.40–4.50) | 2.00 (2.00–2.00) | 2.00 (1.90–2.00) |
| static_resume | 2.20 (2.20–2.20) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| static_discard | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| dynamic_none | 211.85 (210.10–225.50) | 213.35 (209.40–233.20) | 213.25 (207.00–218.80) |
| dynamic_one | 459.35 (450.00–486.80) | 461.95 (452.70–500.70) | 460.60 (453.50–471.60) |
| dynamic_chain_2 | 609.70 (598.60–649.60) | 608.20 (589.60–658.00) | 605.65 (592.10–622.50) |
| dynamic_chain_4 | 977.90 (962.00–1022.80) | 904.80 (888.10–977.70) | 906.35 (895.30–920.20) |
| dynamic_override_none | 214.25 (208.40–225.90) | 213.20 (208.90–222.60) | 213.60 (209.70–219.50) |

O2 同配置的旧／新批次并列，分别汇总，不混合或配对不同时间的进程：

| 路径 | 旧 10000 | 新 10000 | 旧 100000 | 新 100000 |
| --- | --- | --- | --- | --- |
| dynamic_none | 209.35 (203.70–221.20) | 211.35 (205.50–218.20) | 217.05 (208.40–230.20) | 213.35 (209.40–233.20) |
| dynamic_one | 546.10 (523.20–581.80) | 460.25 (447.70–479.40) | 555.15 (542.30–594.00) | 461.95 (452.70–500.70) |
| dynamic_chain_2 | 777.75 (751.80–824.40) | 608.10 (588.90–637.00) | 790.60 (770.30–842.00) | 608.20 (589.60–658.00) |
| dynamic_chain_4 | 1339.35 (1285.40–1388.40) | 909.65 (888.20–940.40) | 1356.15 (1317.60–1401.90) | 904.80 (888.10–977.70) |
| dynamic_override_none | 212.50 (206.60–246.10) | 210.10 (206.10–227.50) | 217.45 (213.00–233.10) | 213.20 (208.90–222.60) |

绑定路径的观察值下降，而 None／局部 None 大体保持原量级；方向与减少每 handler 的
分配相符。但四批是先后观察，不是随机交替的受控 A/B，也不是同一机器状态的重复；
没有 allocator 耗时归因、置信区间、可归因提升倍率、profile 胜者或性能／发布批准。
同配置 O2 静态路径仍约 2 ns，但不同 ABI、harness 与 static_discard 的结果差异不变，
不得相减宣称纯 Slot 指令成本。`approval=none`，没有新增计时阈值。

新长批次 O2 One 三轮中位数为 461.85／464.15／457.20，四成员链为
900.10／906.90／905.30；仍非恒定值。One 的单个批次平均范围为 442.50–544.20，
四成员链为 875.70–1019.30，None 为 203.50–311.80；不是单次延迟尾部上界。
setup 单次观察中位数为 11.349 ms（编译／编码／解码）、24.417 ms（验证加载／JIT）、
0.032 ms（查找、四候选发现、宿主排序、factory、绑定和 context），setup 未固定。
不能把四候选合成 workload 的发现阶段当成通用反射或大 catalog 的性能结论。

下一步优先验证**复用 BindingSet 已固定的 Slot／Contract 身份**，消除每次绑定派发的
额外拥有型身份复制；让现有快照 pin 保持这些记录到 handler 全部返回，不借用宿主
C ABI 字符串、不删除身份／布局验证，也不改公开拥有型 activation 或 payload 寿命契约。
先补调用者修改原始身份、释放 context、嵌套／失败等正确性与分配检查，再匹配协议复测。
这只是下一项候选方案，本轮未实现；不需要新增候选集合机制或关键字。

新证据仍只在本机忽略的构建目录，未自动上传 CI，也不是持久外部存储：
`build-audit-clang64/fragment-evaluation-1081937-i10000-c3-evidence/` 与
`fragment-evaluation-1081937-i100000-c3-evidence/`，每包 74 文件。manifest SHA-256：

```text
10000:  2a1f7ae3f0c85ff4a85d98fd7d3218603172d306ce271e934e487f8360f0fcd8
100000: 1a1d331fcd42077303aab88242aaeecabdf7bb452615ee8b1a6e2fae5745d8fa
```

证据索引 SHA-256：

```text
10000:  e764e8193ab5d3a7a013d416309eb74e93c67dbf9e953aef927ff040312ccd2b
100000: 92070c286c0548b16e7e93da986c873167c39ed5422b83622baf16e82300e174
```

采样前后实际二进制 SHA-256 均为
`bf2b812e51f2ce7264526b41029c27181125cf2d85e44f9cf4563924ed3a8690`；
RuntimeFragment.cpp 均为 `e4772b379dc4e9f59877a8239c46aeb1f5d960b5b545a1851d6901629e4fd554`。
汇总工具仍为 `e35ce22f899950fc79915de09af05d9f4f2eb952a0d082211bc83ff0ca4eeb59`。
这些锚点不是完整构建／二进制归档、签名或可复现性证明；两批旧证据和两批新证据均保持原样。
重跑仍须全新路径，显式 CPU 0、迭代 10000／100000、三轮系列及各自提交／摘要；
后续报告提交不是本轮观察的构建提交，数值也不要求重现。

#### 绑定派发复用固定 Slot 身份（2026-09-27）

已实现上一节的下一项候选：BindingSet 私有 Entry 用拥有型
`RuntimeSlotRequirement` 保存原有两个名义键；查找、稳定分组与局部 override
仍按精确 Slot／Contract 排序。同步链 dispatch 不再复制这两个字符串，activation
引用选中 Entry 的记录。派发入口原有局部 shared snapshot pin 保持 Entry、整链、
环境和 generation，直到全部嵌套 resume／handler 返回；不依赖发布句柄继续存活。

参数 carrier 仍按值拥有 layout／size／alignment／data 记录，payload 存储寿命仍由
宿主负责。C ABI 入口仍复制宿主 C 字符串，公开 activation 仍拥有自己的身份和
carrier；没有改变公开 API／ABI／语法、验证、single-shot 或失败传播协议。
宿主仍明确选择 None／One／顺序链，没有引入自动候选集合、热更新或排序策略。

在旧实现上先加入长身份分配比较，观察到
`bound dispatch copied frozen Slot/Contract identities` 失败；实现后通过。
1／4／64 成员的绑定派发普通 C++ 分配计数现在必须与同参数的 None 派发相等，
不只是随链长保持常数。长 layout carrier 的按值复制仍可能分配，测试不覆盖
所有 aligned／系统分配，也不声称整个派发零分配或给出计时提升。

现有寿命夹具扩展至 768 个组合：One／两成员链、owned／borrowed 环境、BindingSet／
C++ context／C ABI、在首 handler 或 base 修改／销毁调用者身份与 carrier、释放／
替换发布句柄、重复 resume、完成／escape／非法结果／异常。检查所有仍存活的
activation 使用原身份和参数，环境先于 generation 清理，错误不被后续重复 resume
覆盖，替换的 None 只影响下一次调用。同 Slot 的嵌套 1／4／64 链还在最深 base
销毁内外两次调用共用的原始记录，检查最多 128 个悬挂 activation；公开 activation
拥有型 move、handler 异常后恢复与局部 override 原有回归继续保留。

Windows 严格警告普通及 ASan／UBSan 的两个 Runtime CTest 通过；sanitizer 直接
instrument 相关 Runtime 实现。现有 Arch Linux WSL 的 Clang 22.1.8／libstdc++ 16
以 C++17 和 C++23、O0、sized deallocation、ASan／UBSan 直接编译 Runtime 测试
均通过；这不是完整 Ubuntu CI 复刻。Windows 严格警告全量构建成功，77／77 项
非 hardware 回归通过（285.74 秒）；文档 inventory 与 `git diff --check` 通过。

本轮没有新的性能观察系列采样。下一步应在干净的新构建提交上确认跨平台 CI，再用既有 CPU／
迭代／三轮协议及全新证据路径进行匹配采样；不得把上一节数值作为这次身份复用的
性能结果。性能／发布验收和持久外部证据存储仍未完成。

#### 固定身份复用的匹配协议复测（2026-09-28）

观察构建为 `bc9d6fd2c9e760ab3bb10a77d862bffda0e032dc`。确认其 Linux／macOS／
Windows CI 全部成功，run 为 `36326337409`／`36326337446`／`36326337412`。
探针按该提交重建，非计时正确性检查通过；采样前后工作树均干净，期间未改源码或
计时协议。本轮只补观察报告，没有第三项 Runtime 优化；上一实施阶段的 77／77
非 hardware 回归及 Windows／WSL sanitizer 结果保留，不把本次报告提交当成测量构建。

仍在 Windows 11 build 26200、i7-12700（12 核／20 逻辑处理器）、Clang／LLVM 20.1.8、
C++17、严格警告 RelWithDebInfo 原生 `-O2 -g -DNDEBUG` 上观察。按原顺序依次运行
10000／100000 次、各三轮、每路径九个批次、1000 次预热；MoonIR O2、LLVM IR
O0／O2／O3、默认 ORC 与原 harness 不变。每批 54 个新进程、18 个／配置、
4374 个样本，两批合计 108／8748，全部映射、顺序、名义身份、调用数和 checksum
校验通过。只固定测量线程在 group 0／逻辑 CPU 0，样本边界验证亲和性；允许 CPU
0–19，不推定物理核类别，setup 不固定。

电源方案的开始／结束只读查询仍为平衡；没有改变电源、优先级或后台环境，也不控制
频率、温度、后台负载和进程调度。采样期间本代理没有并行运行本机构建、回归、WSL
编译或另一组测量。CPU 固定不是完整机器状态控制，跨日观察尤其不是受控交替 A/B。

与上一版 `1081937` 对应迭代批次的共同 manifest metadata 比较，只有提交与合并
文件摘要不同：probe／workload／runner／validator／亲和性控制器源码摘要、两个
Container 摘要、配置、harness 与 materialization key 相同。两份新 manifest 间只差
迭代与合并摘要。Runtime 实现和实际探针二进制不同；相同 metadata 不等于完整构建
身份或可复现性证明。

以下为**进程中位数的中位数（进程中位数最小—最大）**，单位 ns/op。每个基础样本
是整批调用的平均，不是一次调用延迟。新 100000 次批次完整结果：

| 路径 | LLVM O0 | LLVM O2 | LLVM O3 |
| --- | --- | --- | --- |
| plain | 4.40 (4.30–4.40) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| private_erased | 4.40 (4.30–4.50) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| static_resume | 2.20 (2.10–2.20) | 2.00 (1.90–2.00) | 2.00 (1.90–2.00) |
| static_discard | 1.95 (1.90–2.00) | 2.00 (1.90–2.10) | 2.00 (1.90–2.00) |
| dynamic_none | 208.80 (203.90–217.40) | 207.85 (202.90–213.40) | 209.55 (201.50–221.50) |
| dynamic_one | 404.30 (401.00–408.70) | 404.10 (396.30–422.80) | 404.55 (394.30–425.50) |
| dynamic_chain_2 | 553.80 (542.90–564.80) | 547.80 (542.40–576.60) | 552.35 (540.60–591.30) |
| dynamic_chain_4 | 914.50 (901.40–937.20) | 857.60 (842.30–892.10) | 860.20 (846.80–884.40) |
| dynamic_override_none | 208.60 (203.10–215.90) | 208.40 (204.90–219.90) | 210.55 (202.50–220.00) |

O2 旧／新观察分别汇总，不混合或逐进程配对：

| 路径 | 旧 10000 | 新 10000 | 旧 100000 | 新 100000 |
| --- | --- | --- | --- | --- |
| dynamic_none | 211.35 (205.50–218.20) | 204.80 (198.80–219.10) | 213.35 (209.40–233.20) | 207.85 (202.90–213.40) |
| dynamic_one | 460.25 (447.70–479.40) | 396.65 (386.10–405.50) | 461.95 (452.70–500.70) | 404.10 (396.30–422.80) |
| dynamic_chain_2 | 608.10 (588.90–637.00) | 546.45 (536.00–566.00) | 608.20 (589.60–658.00) | 547.80 (542.40–576.60) |
| dynamic_chain_4 | 909.65 (888.20–940.40) | 850.90 (832.20–880.90) | 904.80 (888.10–977.70) | 857.60 (842.30–892.10) |
| dynamic_override_none | 210.10 (206.10–227.50) | 203.65 (198.40–228.80) | 213.20 (208.90–222.60) | 208.40 (204.90–219.90) |

绑定路径观察值低于上一批，方向与移除每次派发的拥有型 Slot／Contract 复制相符；
但 None／局部 None 也有小幅变化。没有 allocator 耗时归因或受控机器状态，不能把
差值全部归于本次优化，不能宣称可归因倍率、置信区间或 profile 胜者。静态路径约
2 ns，但 ABI／harness 和 static_discard 结果差异仍不允许相减得到纯 Slot 指令成本。
`approval=none`，不新增计时阈值，不据此批准性能或发布。

长批次 O2 One 的三轮中位数为 399.40／404.25／406.80，四成员链为
858.30／856.25／862.70；不是恒定耗时。单个批次平均范围分别为
386.70–443.90／824.10–940.40，None 为 195.40–261.90；不是单次尾部上界。
setup 单次观察中位数为 10.9025 ms（编译／编码／解码）、24.057 ms（验证加载／JIT）、
0.029 ms（查找、四候选发现、宿主排序、factory、绑定与 context）。setup 未固定，
四候选合成 workload 不能支持通用反射／大 catalog 的性能判断。

新证据仍仅在本机忽略目录，未上传 CI 或安排持久外部存储：
`build-audit-clang64/fragment-evaluation-bc9d6fd-i10000-c3-evidence/` 与
`fragment-evaluation-bc9d6fd-i100000-c3-evidence/`。每包 74 文件，索引锚定的
73 个文件字节摘要均通过当前可信 checkout 的检查器；不执行归档脚本或探针。
manifest SHA-256：

```text
10000:  786997de18ad85338efd9191dec544de82e43c1ca3ec2ac28615e3e68cb4d873
100000: 2eec94a483ae2765e2a4bdc49ab9b960e7ca30767f0276e906e82db3736f7ee1
```

证据索引 SHA-256：

```text
10000:  50eecb78d127a8ab2e30e713d74261c5962411e7d08e5f9e20379a3ffa8c561e
100000: 463baafa81aa660aa8c2908e21f6147bdde775491b5e944c0652f76f83dab632
```

采样前后实际探针二进制 SHA-256 均为
`eb8a27e28d66507268c05fb360aa5e46e9cb43c94cd45fc034e2de6799322227`，
RuntimeFragment.cpp 均为
`0812cbdc4f7c931393304f2b383e31bea665e2ae72f558921b1273868a5e0242`；
汇总工具仍为 `e35ce22f899950fc79915de09af05d9f4f2eb952a0d082211bc83ff0ca4eeb59`。
这些不是签名、完整二进制归档或完整构建证明。旧／新证据都未覆盖；重跑必须使用全新
路径及各自提交／摘要，数值不要求复现。

报告校验另从原始 CSV 独立重算两份文档各 47 个表格单元格、轮次中位数、批次平均
范围和 setup 中位数；证据 inventory、摘要锚点、文档 inventory 与 `git diff --check`
通过。这是文档报告阶段，不重新宣称完整编译器回归或新的性能批准。

下一步先收敛 v1 剩余验收清单，区分已完成的核心正确性／分配回归、仍待决的性能／
发布标准与明确延期的能力，不直接追加第三项优化。若继续优化，应先审计 Runtime
入口和参数暂存的拥有／借用及验证边界，提出可检验的分配或开销假设，再做回归和匹配
采样；不删除验证、借用可能在回调中销毁的宿主存储或扩张关键字／自动候选集合机制。
