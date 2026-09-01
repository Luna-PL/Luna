# 基于 Metadata 的版本选择

Luna 不再提供编译器定义的 SemVer 标签、channel、`latest` 或
`name@tag(...)` 后缀选择。版本选择是由类型化 Metadata 和 Selector
构成的普通库策略。

## 声明和附加 Metadata

```luna
meta release {
    channel: string;
    major: i32;
    minor: i32;
    patch: i32;
}

@release("stable", 1, 2, 0)
fn parse() -> i32 { return 120; }
```

`release(...)` 是类型化 Metadata 值。普通附加项只在编译期可见，并会从运行时产物
中移除。Metadata 不改变函数 callable 类型，但会为同名声明提供稳定的候选身份。

## 定义 Selector

静态 Selector 使用真正的编译器内建集合类型：
`declaration_view<T>`、`declaration_ref<T>` 和 `metadata_view<M>`。
它们是 Selector 函数体中的普通类型化值，不是 AST 模式占位符。view 参数显式写在
函数声明中，由 `select` 表达式提供；调用方只填写策略参数。

```luna
fn choose_release(
    candidates: declaration_view<() -> i32>,
    channel: string,
    minimum_major: i32
) -> declaration_ref<() -> i32> {
    let best = declaration_at(candidates, 0);
    let best_major = -1;
    for candidate in candidates {
        for info in metadata::<release>(candidate) {
            if info.channel == channel && info.major >= minimum_major {
                if info.major > best_major {
                    best = candidate;
                    best_major = info.major;
                }
            }
        }
    }
    return best;
}
```

Selector 可以使用条件、循环、局部绑定、赋值、Metadata 字段访问、声明反射和编译期
辅助函数。`select_unique(view, metadata_value)` 仍是可选的精确匹配辅助函数，不是
必需语法，也不是编译器特殊识别的 Selector 函数体。

内建静态 API 包括：

| 操作 | 含义 |
|---|---|
| `for candidate in view` | 遍历有限候选 view 中的所有声明 |
| `declaration_count(view)` / `declaration_at(view, index)` | 统计或索引候选 |
| `metadata::<M>(candidate)` | 获取可迭代的 `metadata_view<M>` |
| `declaration_has_metadata::<M>(candidate)` | 检查是否附加 schema `M` |
| `declaration_id(candidate)` | 反射稳定声明身份 |
| `declaration_signature(candidate)` | 反射稳定 callable 类型身份 |

编译器不解释 Metadata schema 的含义，也不强加 SemVer 策略。Selector 执行后，编译器
只验证返回的 `declaration_ref<T>` 属于输入 view。缺失返回、view 外引用或不可求值
的静态 Selector 都是编译错误。

## 静态选择

正式语法和前缀简写等价：

```luna
let f = select parse with choose_release("stable", 1);
let g = @choose_release("stable", 1) parse;

f();
@choose_release("stable", 1) parse();
```

静态选择在 MoonIR 验证前完成。MoonIR 保存被选中的声明身份，因此运行时没有选择
成本。候选超过一个时，无限定调用会被拒绝；编译器不会按源码顺序或隐式 latest 规则
选择声明。Metadata 适合开放声明族，但不是必需条件；Selector 也可以使用签名、声明
反射或函数体可用的其他编译期策略。

## 直接静态声明反射

当普通名称和签名已经唯一确定声明时，不需要 Selector：

```luna
let known = declaration_of::<(i32) -> i32>(parse);
print(declaration_id(known));
print(declaration_signature(known));
```

`declaration_of` 产生只存在于编译期的 `declaration_ref<T>`。静态反射折叠后它会被
擦除，不创建运行时 descriptor。若名称和可选签名仍对应多个声明，编译失败；开放边界
应使用 `select`。

## 运行时可见性与宿主绑定

当运行时操作必须检查 Metadata 时，在附加项前加 `runtime`：

```luna
runtime@release("stable", 1, 2, 0)
fn parse() -> i32 { return 120; }
```

`runtime@...` 也为声明提供最小 Runtime Descriptor。静态操作不会隐式保留
Metadata、descriptor、Selector 代码或反射数据；`runtime` 必须显式书写。纯编译期
Metadata 不能通过 Runtime descriptor 检查。原 `dynamic` retention 与
`dynamic select` exact-match 协议在 0.3 中被拒绝；runtime 切换由 typed EV004 宿主
binding 承担。

## 当前边界

Metadata 附加项可用于函数、fragment、struct、enum、trait 和 impl。当前可执行的
`select` 表达式绑定 callable 函数族。类型、trait 和 fragment 族选择将复用同一
Selector 组件和 declaration-view 协议，不重新引入特殊版本语法。

参见 [examples/versioning.luna](../examples/versioning.luna) 的静态选择，以及
[宿主进化 API](evolution.zh-CN.md)的显式 runtime binding 与 generation switching。
