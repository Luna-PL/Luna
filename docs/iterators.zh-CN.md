# 迭代、管道与容器边界

Luna 当前的目标是“动态扩展 + 有限控制流增强 + 系统语言性能”。迭代机制因此采用
惰性、可融合的普通控制流模型，不引入完整代数效应，也不建立独立的 effect summary。
实现与安全事实继续由编译器推导到只读 `sysmeta`。

## 数组与借用切片

`array<T, N>` 是定长、内联存储的值类型，`N` 必须是编译期非负整数。数组字面量
推导元素类型和长度，所有元素必须具有同一类型：

```luna
let values: array<i32, 4> = [10, 20, 30, 40];
values[1] = values[0] + 5;
```

索引只接受整数。常量越界在编译期拒绝；动态越界由运行时检查并终止，不能形成未定义
的越界访问。

`values[start..end]` 创建只读 `slice<T>`，等价于
`slice(borrow values, start, end)`。切片是 `{data, length}` 的非拥有借用视图，
使用半开区间并要求 `start <= end <= length`。切片存活期间来源数组不能写入，切片
索引也执行动态边界检查。当前稳定表面没有 `slice_mut<T>`；`raw<T>` 和
`device_buffer<T>` 不会隐式变成数组或切片。

## 当前可用表面

array 提供全部三种所有权模式：

```luna
values.iter()       // Iterator<&T>
values.iter_mut()   // Iterator<&mut T>
values.into_iter()  // Iterator<T>，消费数组时按值转移元素
```

只读 slice 仅提供 `view.iter() -> Iterator<&T>` 和直接的共享迭代。它不能产生可变引用，
也不能转移元素所有权；这两类操作要求 owning array receiver。

整数半开区间使用 `range(start, end)`。惰性适配器包括：

```luna
iterator.map(fn(T) -> U)
iterator.filter(fn(T) -> bool)
iterator.take(count)
```

终结操作包括：

```luna
iterator.fold(initial, fn(Acc, T) -> Acc)
iterator.for_each(fn(T) -> unit)
iterator.count()
iterator.collect::<Target>()
```

`for item in iterator { ... }` 也是终结点。直接遍历数组按值产生元素；直接遍历切片按
共享引用产生元素。`range` 是半开区间，所以上界不包含在结果中。

用户类型可实现 Core 协议并直接进入 `for`：

```luna
impl core::iter::Iterator<i32> for Counter {
    fn next(iterator: &mut Counter)
        -> core::option::Option<i32> {
        // 返回 Some(item) 或 None
    }
}

let iterator = new Counter(...);
for item in iterator { ... }
```

普通成员调用使用静态 trait 分派，所以 `iterator.next()` 和
`(move collection).into_iter()` 都解析到唯一的具体 impl symbol，没有 vtable。
`for item in collection` 会在 collection 本身不实现 Core `Iterator` 时，静态查找
唯一的 Core `IntoIterator` impl，消费局部 collection，并且只调用一次
`into_iter`。编译器持有转换产生的隐藏 iterator 状态，正常遇到 `None` 或从函数
提前 `return` 时都会执行相应清理。协议源目前必须是局部绑定。

`collect::<Target>()` 静态选择目标类型唯一的 Core `FromIterator<Item, Builder>`
impl。协议不是接收一个可逃逸的 iterator 对象，而是：

```luna
impl core::iter::FromIterator<i32, SumBuilder> for CollectedSum {
    fn begin() -> affine SumBuilder {
        return new SumBuilder(0);
    }

    fn push(builder: &mut SumBuilder, affine item: i32) -> unit {
        builder.total += item;
    }

    fn finish(affine builder: SumBuilder) -> affine CollectedSum {
        let total = builder.total;
        return new CollectedSum(total);
    }
}
```

编译器在融合循环前调用一次 `begin`，对每个通过 adapter 的元素调用一次 `push`，
最后调用一次 `finish`。目标类型必须用 turbofish 显式给出；Sema 会检查精确的 Core
trait identity、Item 一致性、完整方法集和 affine 所有权契约，不使用 vtable。

## 实现模型

适配器链在前端形成编译器域的临时 iterator recipe。在 canonical CFG 构造中，`for` consumer 会把
已验证的源、适配器顺序、元素输入输出类型和借用模式展开为一个普通循环。它不会为
`map` 或 `filter` 建立中间数组，不依赖 `Vec`，也不调用 iterator runtime ABI。

在 canonical CFG 构造中，array 提供编译期常量上界，slice 则在 loop init 中提供一个
经验证的 `usize` `SliceLengthExpr`。该节点是 slice 的基本投影而非 iterator operation；
source 与它的运行时上界都只求值一次。

无捕获 recipe 现在可绑定到局部变量。绑定时按源代码顺序
立即求值源、lambda 函数指针和 `take` 参数，并在栈上保存源指针/Copy 快照、索引、
边界及 adapter 状态。绑定值可由 `for`、`fold`、`for_each`、`count` 或 `collect`
消费，也可
在消费表达式中继续追加 adapter；整个链仍静态展开为一个循环，不分配 heap，不使用
vtable 或 iterator runtime ABI。

对 canonical `for` 构造而言，materialized Iterator binding 会在绑定点被擦除为普通的
source/index/limit/adapter locals。index 同时是 affine 单次消费凭证，并在最终循环中通过
move 转移；这不增加独立 runtime token。materialized `count` 和 Copy accumulator
`fold` 现在会从该状态继续展开为带外层结果 local 的普通循环；表达式语句位置的
`for_each` 会在循环体生成普通 call。在 terminal 上追加的 adapter 会在 terminal 参数之前
求值一次。这个首个表达式子阶段接受作为直接 initializer/return 的有值 terminal，
或将其作为直接普通 call 的唯一参数；`for_each` 只接受 expression statement 位置。
因而它不会重排更早的 sibling operand。

这种 binding 是 affine、single-consumption 的局部值，当前不能返回、传参或跨 ABI。
借用型 binding 将源 loan 保持到词法作用域结束；Copy `into_iter` 在绑定点建立值
快照。move-only `into_iter` 在绑定点把源所有权转入隐藏栈状态，并为每个数组元素
保存初始化位。终结消费、`for` 中提前 `return`、普通函数提前 `return` 以及从未
消费就离开作用域，都会只清理仍初始化的元素。

non-materialized terminal、通用 expression sibling hoisting、affine fold accumulator 和 `collect`
仍是明确的 canonical-CFG 边界。其中 `collect` 必须等 affine `FromIterator` builder、可变
`push` borrow、cleanup path 与 consuming `finish` transfer 都能表示为普通且可验证的状态后
才会跨过该边界；不会用隐藏 iterator runtime object 作为捷径。

源表达式、适配器参数和终结参数按从左到右的源代码顺序求值。`filter` 跳过元素，
`take` 只计算流经它的元素，因此适配器顺序具有通常的惰性管道含义。

## 所有权

- `iter()` 在消费期间持有共享借用。
- `iter_mut()` 在完整 `for` 循环期间持有独占可变借用。
- 用户 Core Iterator 的状态在完整循环期间持有独占可变借用，`next` 只能通过已
  解析的 `&mut Self` 协议入口推进。
- 用户协议允许 move-only `Item`。`Some` tag 是该轮的初始化状态；元素绑定在每次
  成功迭代后重新初始化。正常走到循环体末尾时执行一次 `Drop`，函数 `return` 等
  提前退出路径由路径敏感 cleanup 执行一次 `Drop`，移动走的元素不再清理。
- 数组递归继承元素的 Copy/affine/linear 与 Drop 属性。构造 move-only 数组时每个
  元素必须显式 `move`；直接消费数组或调用 `into_iter()` 后由隐藏数组快照和逐元素
  初始化位记录所有权。已交付的槽位不再由数组清理，`take` 留下的尾部以及函数提前
  `return` 时尚未交付的槽位会恰好清理一次。隐藏 linear 状态仍被拒绝。
- `filter` 可处理 move-only 元素，但 predicate 必须共享借用该元素；被拒绝的元素
  立即清理。`take` 可处理 move-only 元素，达到上限时清理当前元素，并在循环退出后
  清理尚未读取的源槽位。
- lambda 函数体现在使用与普通函数一致的路径敏感所有权检查。owning affine 参数会
  在 fallthrough 和每条 `return` 路径清理，也可通过 `-> affine T` 返回契约继续
  转移；未消费的 linear 参数会被拒绝。
- `map` 可消费 move-only 输入，但 transform 参数必须显式 owning；它可以产生 Copy
  或 move-only 输出。输出随后被 `filter`/`take` 拒绝、交给 `for`，或通过另一个
  owning adapter 时都有明确所有权。
- `for_each` action 与 `fold` reducer 可接收 move-only item，但相应参数必须显式
  owning。`count` 会在计数后直接清理 move-only item。
- `fold` 支持 affine move-only 累加器：局部初值必须显式 `move`，reducer 必须
  owning 接收旧累加器并返回 replacement 的所有权。编译器在每轮调用前清除
  accumulator initialized bit，写回新值后重新置位，最后把结果转移给调用者并清除
  状态。忽略 move-only fold 结果会被拒绝。linear 累加器暂不隐藏在该状态中。
- `fold`/`for_each`/`count` 终结表达式会消费 move-only 局部数组源，在 MoonIR 中
  携带唯一的隐藏 recipe 状态，并在产生结果前清理尚未交付的槽位。终结后再次使用
  源会被判定为 use-after-move；linear 源不能隐藏在该状态中。
- `collect` 使用相同的终结 recipe/drop-state。每个通过管道的 item 的所有权转移给
  `FromIterator::push`；`take` 截断、`filter` 拒绝以及源尾部仍按原规则恰好清理
  一次。builder 只存在于当前函数的隐藏栈槽，`finish` 消费它并把 affine 结果交给
  调用者；忽略该结果会被所有权检查拒绝。
- 适配器 lambda 当前必须无捕获；引用外层局部变量会得到明确诊断，直到 closure
  environment 布局完成。设备 kernel 中的管道尚未开放。
- materialized recipe 是 affine 单次消费值；第二次 `for` 或终结调用是
  use-after-move。`iter_mut()` binding 在整个词法生命周期保持独占源 loan；
  Copy `into_iter()` binding 则与绑定后的源修改相互独立。拥有 move-only 数组的
  binding 在创建时使原源成为 moved；隐藏的 `[N x i1]` Drop 状态在交付元素前清除
  对应位，并在消费结束或任意作用域退出路径清理尾部。

这些约束是阶段性安全边界，不是最终的 Iterator 抽象能力。

## 容器是否是 builtin

迭代协议不要求动态容器成为 builtin。固定数组和借用切片仍是语言基础类型，因为其
长度、布局、边界检查和借用关系直接参与类型检查与代码生成。未来的 `Vec<T>`、链表、
哈希表及用户容器应位于 Core/标准库，通过稳定的 `IntoIterator`/`Iterator` trait
接入；编译器只保留必要的内建入口和优化识别。

目前已物化稳定核心声明：

- `org.luna.core::option::Option`
- `org.luna.core::iter::Iterator`
- `org.luna.core::iter::IntoIterator`
- `org.luna.core::iter::FromIterator`
- `org.luna.core::iter::{Map, Filter, Take}`

当前编译器 recipe 不通过运行时反复调用 `next() -> Option<T>`，因为静态已知链可以
直接融合。局部物化只保存静态展开所需的栈状态，并不自动改写成 Core `Map`/`Filter`/
`Take` enum 或 trait 调用。Core `Iterator::next(&mut Self) -> Option<Item>` 仍定义
用户协议；跨函数、跨 package 的 adapter 值最终应使用这些稳定 Core 类型。

用户协议路径现已接入：Sema 只识别具有稳定 package identity 的
`org.luna.core::iter::Iterator`，验证 `next(&mut Self) -> Option<Item>`，
并把唯一的静态 `next` symbol、iterator 类型、Option 类型和 variant index
写入 MoonIR。LLVM 每轮调用一次 `next`，按 `None`/`Some` tag 分支。一个形状相同、
同样名为 `next` 的其他 trait 不会被误认为迭代协议。

这条路径与 compiler recipe 有意并存：已知数组、切片和 range 继续融合；普通用户
状态机使用协议调用。Core `IntoIterator` 已可通过静态成员调用生成 iterator，
并且 `for` 会在源不直接实现 Core `Iterator` 时隐式插入唯一的静态转换。Sema 将
转换 symbol、输入类型、隐藏状态名和清理事实写入 MoonIR，LLVM 不再重复 trait 查找。
Core `FromIterator` 也已接入 compiler recipe，但采用 `begin/push/finish` builder
协议，因此不要求先把 recipe 转成跨 ABI `Iter`。MoonIR 保存目标和 builder 类型及
三个唯一 impl symbol；LLVM 仍生成单循环且不建立中间容器。

当前 `FromIterator` impl 必须是 concrete impl；泛型 impl 要等 impl specialization
进入 coherence 后开放。Item、builder 和 target 目前不得为 linear，且
`collect` 仍只消费当前的局部 compiler recipe。动态 `Vec<T>` 等容器仍应由标准库
提供自己的 builder 和存储策略，而不是升级为 builtin。

## 与效应和 sysmeta 的关系

迭代管道是普通、有限、结构化控制流，不是 continuation handler，也没有恢复或多次
恢复语义，因此不应被建模成代数效应。当前 recipe 的 host-only 能力是编译器推导的
`sysmeta` 事实。后续若开放设备 lowering、可暂停迭代器或协程适配器，编译器应继续
推导 `maySuspend`、存储和 ABI 事实，而不是要求用户维护平行的 effect summary。

## 后续演进顺序

1. 把当前编译器内部的 materialized Drop 状态映射到稳定 Core inline adapter
   布局，随后才允许 adapter 作为普通值跨函数或 package 边界。
2. 完成 closure environment 的所有权、借用和 Drop 布局后允许捕获式
   `map`/`filter`。
3. 在 coherence 支持 impl specialization 后开放泛型 `FromIterator` impl，并由
   标准库动态容器实现实际 builder。
4. 最后再与无栈协程对接异步迭代，并由 `sysmeta` 推导暂停和 frame 需求。
