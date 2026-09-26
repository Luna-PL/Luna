# Slot/Fragment 运行时注入计划

[English](slot_fragment_runtime_plan.md) | 简体中文

> 状态：已确认实施计划，2026-09-23
> 范围：类型安全、由宿主控制的运行时注入

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

静态 apply 仍可特化为零 Runtime 成本。绑定或导入 Fragment 会创建
`RuntimeFragmentRef<S>`；它拥有或借用显式环境，并固定所属 module generation。
`apply` 借用该引用，每次 Slot invocation 都创建新的 single-shot activation。

## 宿主控制的发现与注入

目标指向 exported Slot 的 exported Fragment，凭名义关系成为候选；metadata 不授予
候选资格。已验证产物发布只含数据的 `FragmentOffer`，至少携带 FragmentId、目标
SlotId/ContractId、执行/factory 契约、环境布局、generation identity 和保留的策略
metadata。

Runtime 对外保证强类型候选目录的语义：

```text
candidates(SlotRequirement{SlotId, ContractId}) -> CandidateSnapshot<S>
```

规范不要求急切维护全局索引。Runtime 可以扫描、缓存、惰性建索引或合并各 generation
目录，只要快照完整、确定、不可变并固定 generation。

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
`select` 调用的 exported 编译期 selector 现于声明阶段分类，并从 MoonIR 擦除。当前 lowering
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
