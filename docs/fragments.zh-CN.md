# Interceptor、Context 与 Slot

Luna 0.3 把控制挂接点表示为模块级名义 `slot`，把实现表示为显式指向该 slot 的
`interceptor` 或 `context`。slot 调用必须携带词法 continuation body，`apply` 也必须
携带词法 body。

```luna
slot interceptor observed(value: i32) default audit;

interceptor audit(value: i32) for observed {
    print(value);
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

Slot 与 fragment 都在模块作用域声明。fragment 必须绑定 slot 的全部参数，并保持相同的
类型、所有权 relation 和 usage；控制形式也必须与目标 slot 一致。目标 slot 为两项声明
提供名义契约身份，因此两个结构完全相同的 slot 仍是不同类型。`default` 绑定不受声明
顺序影响。

## Single-shot 控制契约

Luna 0.3 冻结 unit-result、single-shot 控制。`context many`、dynamic slot/apply、局部
slot 声明和无 body 的 apply 均以迁移诊断拒绝。

| 操作 | `interceptor` | `context` |
| --- | --- | --- |
| 自然落尾 | 恰好一次进入 continuation | 未消费时丢弃 continuation；执行过 `resume()` 后则完成 fragment |
| `resume()` | 拒绝 | 进入 continuation 一次；若其正常完成，再回到 fragment 的下一条语句 |
| `return;` | 结束 fragment 并跳过 continuation | 结束 fragment 并跳过尚未消费的 continuation；若已 resume，则结束 post-resume fragment 代码 |
| `abort()` | 显式丢弃 continuation | 显式丢弃尚未消费的 continuation |

Fragment return 只能是 unit，`return value;` 会被拒绝。single-shot context 在
`resume()` 后执行 `abort()` 也会被拒绝，因为 continuation 已被消费。context 路径可以不写
`resume()`；自然落尾表示隐式丢弃，不是“缺少控制操作”错误。

在进入 continuation 之前，`return;` 与 `abort()` 可能抵达同一后继，但它们仍是不同的
canonical 操作：return 是 fragment-local 的正常终止；abort 记录显式丢弃 continuation 的
决定，并且不能用于已经消费 continuation 的 single-shot 路径。

## Continuation 边界

词法 body 属于调用函数，而不属于 fragment：

- continuation 中的 `return value;` 返回外层函数，并跳过 context 中 `resume()` 之后的代码；
- continuation 中的 `?` 从外层 `Result` 函数传播，具有相同的跳过行为；
- fragment 内的 `?` 会被拒绝，因为它会隐式跨过 slot 边界；应在 fragment 内显式处理
  `Result`；
- fragment 局部名在 continuation 中不可见，即使它遮蔽了调用作用域中的同名变量。

因此 `return` 和 `?` 不会因所在位置而获得第二套隐藏含义。

## 所有权与 cleanup

每条离开 fragment 的边都携带显式 cleanup 义务：

- interceptor 局部值会在自然转发、`return;` 或 `abort()` 抵达目标前清理；
- context 局部值跨 `resume()` 存活，并在 context 退出时清理，包括 continuation 从外层函数
  `return` 或用 `?` 传播时；
- continuation 局部值按普通函数/块退出规则清理；
- fragment-local 退出不会隐式消费外层资源；所有能抵达 slot 之后代码的路径必须具有一致的
  ownership、borrow 与设备 in-flight 状态；
- fragment 中仍有效的 linear 局部值必须在 fragment 退出前被消费。

Cleanup 顺序记录在 canonical CFG edge 上，并在代码生成前验证。静态组合因此不需要堆上
continuation，也不需要 runtime dispatch。

## Apply 与默认实现

`apply fragment { ... }` 从 fragment 声明推导目标 slot。在词法 body 内，相应 slot 调用
使用该 fragment；body 外使用 slot 声明的 default。既没有活动绑定也没有 default 的 slot
按 identity 操作处理，直接执行 continuation。

```luna
slot context measured();

context profile for measured {
    let start = monotonic_now();
    resume();
    print(monotonic_now() - start);
}

fn run() -> unit {
    apply profile {
        measured() {
            perform_work();
        }
    }
}
```

导出的 slot 与 fragment 可按普通 package/module 规则使用限定名。`symbols(slot_name)` 可以
查询 slot 声明，但 slot 只是可反射、不可调用的声明元数据，不是函数值。

## Runtime 边界

Runtime-retained Slot/Fragment descriptor 已具有稳定 declaration kind、名义 ID、contract
ID，以及 fragment 指向 slot 的强引用。这冻结了 loader 与 tooling 所需的表示，但没有创造
第二套源码语言。当前这些 row 只是不可调用的 descriptor/identity 证据，不含
runtime continuation entry；0.3 是否冻结该承诺及 exported/private slot 的保留规则，由
`TBD-SF009` 决定。

Luna 0.3 尚不公开 runtime typed-reference 的查找/获取语法。`apply fragment { ... }` 是唯一
apply 拼写；0.3 是否保持 static-only 由 `TBD-SF007` 决定。typed reference 的获取、
所有权、生命期与 runtime apply ABI 归入 `TBD-SF010`。
已移除的 `dynamic slot` 与 `dynamic apply` 只保留为迁移错误 corpus；原 external plugin ABI
和环境变量驱动的 dispatch runtime 已删除，不进入 0.3。
