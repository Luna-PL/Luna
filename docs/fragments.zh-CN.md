# Slot 与 Fragment

[English](fragments.md) | 简体中文

Luna 使用模块级、名义化的 `slot` 表示注入点，使用显式目标指向该 Slot 的
`fragment` 表示处理器。声明 Slot 本身就是对受控切入的显式选择；Slot 不是函数或可传递
值。Fragment 可以通过 `resume` 获得编译器拥有的 Slot 续体，因此不能与 function
reference 互换。

```luna
slot observed(value: i32);

fragment audit(value) for observed {
    print(value);
    resume;
}

fn main() -> i32 {
    apply audit {
        observed(41) {
            print(42);
        }
    }
    return 0;
}
```

Slot 与 Fragment 都是模块级声明。Fragment 绑定 Slot 的全部参数，并继承目标的类型与
ownership contract。目标关系是名义化的：两个形状相同的 Slot 仍是不同注入点。

## Single-shot continuation

首个契约只支持 unit-result、single-shot。`resume;` 最多一次进入宿主选定的下一个
Fragment，或 invocation 的 base continuation；continuation 正常结束后，控制流回到
`resume` 后的 Fragment 语句。Fragment 在 `resume` 前自然落尾或执行 `return;`，都会丢弃
尚未消费的 continuation。

```luna
fragment measured[label: i32](value) for observed {
    print(label);
    let start = monotonic_now();
    resume;
    print(monotonic_now() - start);
}
```

Continuation 属于发起调用的函数：

- continuation 中的 `return value;` 返回该外层函数，并跳过 post-resume Fragment 代码；
- continuation 中的 `?` 具有相同的外层传播行为；
- continuation 不能被保存、返回、伪造或恢复两次；
- Fragment 局部名称对 continuation 不可见。

Cleanup obligation 显式记录在 canonical CFG edge 上。跨 `resume` 存活的 Fragment 局部
值会在正常结束和外层逃逸时得到清理。

## Apply

`apply fragment[环境实参] { ... }` 在词法范围内构造并安装 Fragment。方括号只表示
Fragment 构造环境，圆括号始终只表示 Slot invocation 契约。环境实参在进入 apply region
时求值一次，后续每次匹配的 Slot invocation 都复用它。Fragment 正文不会隐式捕获 apply
调用点的局部变量。Apply region 拥有 Copy 或 affine 环境字段，每次 Fragment activation
只获得 shared-borrow 视图。因此 affine place 在构造时必须显式 `move`，拥有型右值则可
直接传入。linear 字段会被拒绝，因为可复用的借用环境无法证明恰好消费一次。字段所有权
由其类型推导，所以环境参数列表不接受所有权修饰。Slot 没有 active binding
时，直接运行 invocation 提供的 base continuation。

```luna
apply measured[7] {
    observed(10) {
        perform_work();
    }
}
```

静态组合可以内联，不携带 Runtime descriptor 或 dispatch 成本。已确认 runtime 计划会把
同一 operand 位置扩展为已验证的 `RuntimeFragmentRef<S>`，而不是增加 `dynamic apply`。

## 公开候选

`export slot` 发布稳定的注入契约。目标指向 exported Slot 的 `export fragment` 是该确切
SlotId 的候选实现。候选资格来自已验证的名义关系与 ContractId，绝不来自用户 metadata。

通过公开 schema 附加的 metadata 是宿主选择策略。typed candidate 查询现在按精确
SlotId/ContractId 返回公开且可执行 Fragment 的不可变、generation-pinned 快照；宿主负责
为每个精确 Slot 选择 None 或一个候选、构造不可变 BindingSet，并且只在 Runtime safe point
发布。普通 Slot dispatch 不运行反射、metadata 过滤或 descriptor 重新验证。

Descriptor、生命周期、宿主策略和分阶段实现边界见已确认的
[Slot/Fragment 运行时注入计划](slot_fragment_runtime_plan.zh-CN.md)。

## 实现状态

统一的 `slot`、`fragment ... for`、`resume;` 语法已经接入 single-shot 静态 Sema、
MoonIR、JIT 与 AOT 路径。旧分类、`many`、`abort`、声明 default 与 Slot/Fragment 的
`runtime` 修饰均已退出合法源码表面，相应的语义字段与 lowering 分支也已经删除。0.3
容器仅把旧 wire offset 保留为 canonical reserved value，并在解码时拒绝 legacy 值。

Runtime Fragment factory descriptor 与 `RuntimeFragmentRef<S>` 已具备宿主 ABI 基础；
源码侧显式 Copy 与 affine 环境已经接入 Sema 与 MoonIR，并在 Apply 正常退出、外层
`return` 和 `?` 传播时执行 cleanup。导出的控制声明现在携带独立于 retention 的
`PUBLIC_CONTROL` descriptor 能力；精确 Fragment 目标/环境事实会经过容器验证加载保留，
candidate snapshot 也会按精确 Slot contract 过滤公开可执行绑定。Slot argument record 与
Runtime-owned opaque single-shot activation 也已冻结。首版公开执行 ABI 接受 Copy Slot 参数
契约和 Copy Fragment 环境；静态 apply 仍支持 affine 环境。exported Fragment 现在通过容器
可达的隐藏函数复用静态 CFG 组合，LLVM 发射经过验证的 factory/destroy/execute wrapper，并将
其发布为可执行候选。不可变 BindingSet 构造、safe-point 原子激活、pinned snapshot、
None/One dispatch、宿主定序 chain 和不可变局部 Slot override 已实现。显式 execution-context
C ABI 可在不依赖全局/TLS Runtime 状态的前提下传播 continuation escape；artifact-cost 门禁也
已覆盖 exported executable 物化与 static composition 擦除。未静态绑定的 exported Slot invocation
现在会在 sealed CFG 中保留为经过验证的 `RuntimeSlot` terminator，显式携带确切 declaration
reference、冻结的参数 Record TypeId、已打包 operand、continuation 入口与完成边。静态 `apply`
composition 仍优先于该路径，未绑定的 private Slot 仍可擦除。effect-directed context 传播、
包括由 verifier 重算的精确 direct call 最小不动点，现在已表示并以
`requires_fragment_context` 写入容器。受影响的内部 LLVM function 现在已增加一个前置隐藏
context 参数，并由精确 direct call 转发；
不受影响的函数保持原 ABI。需要 context 的 `runtime fn` 现在会发布带
`FRAGMENT_CONTEXT` ABI flag 的普通 Function descriptor，其 binding 也保留该 flag，供宿主做
typed lookup；这直接复用现有 runtime catalog，不另建入口注册表。普通 `export fn` 仍承诺源码
声明的公开 ABI，因此需要隐藏 capability 时会被拒绝；需要 context 的 function value 也继续等待
context-aware indirect-call ABI。continuation dispatch 现在会把同步 capture outline 到显式
栈 frame，打包冻结参数 record、调用稳定 Runtime ABI，并在沿 completion edge 继续前回写 capture
修改。外层 `return`/`?` 会通过 frame 和独立 escaped status 传播；canonical cleanup edge、
Result switch 与 case binding 都在 callback 内先执行再逃逸。测试也已让宿主选择真实生成的
Fragment，并经其 `resume` 进入该 callback。continuation 调用另一个会触发动态 Slot 的函数
已经可运行，显式 context 会继续转发。词法嵌套的 Slot 现在会递归 outline callback、
传递同一 context 和 return storage，并回写跨层 capture，包括 affine/resource。内层逃逸会先
执行封存的外层资源 cleanup，再传播返回；若宿主选中的 Fragment 不调用 `resume`，则走 Slot
后续路径的 cleanup。新增的结构化 IR 门禁会统计动态 dispatch，并检查静态
组合是否完全擦除 runtime 选择成本。
