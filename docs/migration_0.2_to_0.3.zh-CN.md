# Luna 0.2 源码迁移至 0.3

[English](migration_0.2_to_0.3.md) | 简体中文

> 文档类别：迁移记录
> 来源：Luna 0.2.1
> 目标：候选 Luna 0.3.0
> 状态：Draft；由 `TBD-*` 标识的目标拼写有意不在本文猜测
> 冻结的 0.2 源码/编译器检查点：`a188d87a6f10d7fa67582389a0a0b915f3741401`

Luna 0.3 明确断代。0.3 编译器没有 package language selector、edition、兼容 flag 或
旧 lowering 路径。未改动的 0.2 源码应继续使用 0.2 编译器；一个 package 应作为一次
显式操作完成迁移。

## 破坏性变化登记表

| 领域 | 0.2.1 源码/模型 | 0.3 源码/模型 | 变更理由 | 迁移动作/状态 |
|---|---|---|---|---|
| 编译器选择 | 0.2 编译器接受 0.2 语言 | 0.3 编译器只接受 0.3 | 双轨语义会永久扩大编译器 | package 完成迁移前固定旧编译器；不增加 `language=` |
| 阶段/retention | CompileTime、Runtime、Dynamic retention；`dynamic select`、`dynamic slot`、`dynamic apply` | 仅 compile-time/runtime；运行时行为由 value、descriptor、query 和 builtin 表达 | Dynamic 混合 retention、discovery 与 dispatch | replacement 冻结后删除 Dynamic form；query 顺序见 `TBD-Q004`，Slot/Fragment 源码拼写见 `TBD-SF006` |
| 具名类型默认 | named struct/enum 默认结构，除非声明 `nominal` | named declaration 默认获得名义 TypeId；`nominal` 不再是关键字；匿名 record 使用不带 `record` 关键字的 `{ field: Type }` / `{ field: value }` | 稳定 package/runtime identity 不能依赖偶然相同的 layout，冗余 modifier 也会徒增 grammar | 将 `nominal struct/enum` 改为普通 `struct/enum`；用 `Target { field: value }` 显式构造、用 `{ field: source.field }` 投影，或证明显式 constraint/shape relation；`TY002` 已确认 |
| Ownership usage | binding 级 `affine`/`linear` declaration | 前置 `copy let`/`affine let`/`linear let` contract + 已实现的 `affine {}`/`linear {}` default block | 块糖减少资源代码噪声且不增加运行时状态 | 将任何写在 `let` 之后的 qualifier 移到前缀；可选地用 usage block 分组 binding；Sema 固化并验证 binding 后擦除 block policy |
| Rc/Arc | 编译器特判 `rc new T(...)`、`arc new T(...)` 与专用 TypeKind | 已实现普通名义 Core `Rc::new(value)`/`Arc::new(value)` 容器 | 删除跨编译器特判，让库负责容器策略 | 导入 `org.luna.core`，显式构造并通过 `Clone` 复制 handle；旧拼写在 0.3 中直接拒绝 |
| 布局反射 | `type_size::<NamedStruct>()` 返回源字段大小之和，与实际指针值 ABI 不一致 | `type_size`/`type_alignment` 报告真实值槽 ABI | 泛型容器必须按真实 size/alignment 分配和加载 | 不要用 `type_size` 估算具名 product 的字段 storage；当前 64 位 pointer-represented struct 值为 8/8 |
| Slot/Fragment | 函数内 slot statement、结构 contract、`apply name(fragment)` 和动态有限候选 | 模块级二等 slot identity、名义 Fragment target、静态 MoonIR composition、typed RuntimeFragmentRef | local string 与结构 shape 无法支撑安全开放扩展 | 旧形式直接拒绝；`TBD-SF006` 冻结声明和 control 行为后迁移 |
| MoonIR | pointer-heavy 内存 IR；`--emit-moonir` 是诊断输出 | 内存/序列化共用的单一 canonical table-referenced MoonIR | Moon Container 需要稳定 identity 与验证，且不应引入第二个 IR | framing、八段 payload、canonical CFG code codec、原子验证 loader 和生产 JIT 往返均已实现 |
| 产物输出 | build/check flag 暴露当前 executable 与诊断 MoonIR 路径 | manifest `kind` + 已确认的 `-t native`（默认）、`-t moon`、`-t cffi`；正式 build 必须使用 package | 单一输出维度使源码语义与 packaging 解耦 | 增加 package `kind`；按已确认 `T003` 迁移输出路径；CFFI 使用 `export "C" fn`，Moon foreign dependency 使用 `[host-imports]` |
| Native/foreign 信任 | 当前 Runtime/plugin descriptor 不能证明完整 native artifact | Moon 本地验证，Luna Native 由绑定 code/data 的证明建立信任，C FFI 仍为 foreign/unsafe | typed header 不能单独认证 implementation byte | Luna native library 重新生成证明；未证明库显式通过 `extern "C"` 接入；没有通用 `unsafe {}` |
| 进化 | 没有 Moon Container generation loop | host-specific Moon Container staging、验证、安全点 activation 与 rollback | 进化必须显式、受验证且不进入普通调用热路径 | 公开 binding 拼写等待 `TBD-EV004`；0.3 不迁移持久状态 |

任何一行都不授权在 0.3 编译器中加入兼容代码。目标拼写仍为 TBD 时，正确的临时动作是
继续使用已冻结的 0.2 编译器，而不是猜测语法。

## 冻结的 0.2 迁移 corpus

[`tests/migration_0_2_baseline.cmake`](../tests/migration_0_2_baseline.cmake) 登记十个代表性
案例，覆盖结构化具名类型、结构 generic reuse、动态 symbol/fragment selection、局部
Slot/Fragment/apply 和编译器特判 Rc/Arc。它复用既有 fixture，避免迁移证据与完整 0.2
回归套件演变成互相竞争的副本。

普通 0.3 测试配置只校验 manifest 及其 source fixture 仍然存在。要复现 0.2 语义，应在
独立 build directory 构建冻结提交，然后运行：

```sh
cmake \
  -DLUNA_SOURCE_DIR=/path/to/luna-0.2-source \
  -DLUNA_0_2_EXECUTABLE=/path/to/luna-0.2-build/luna \
  -P /path/to/luna-0.2-source/tests/migration_0_2_baseline.cmake
```

runner 会拒绝 analysis identity 不是冻结提交的编译器。这样即可独立复现旧证据，而不在
0.3 binary 内携带任何 0.2 branch。
