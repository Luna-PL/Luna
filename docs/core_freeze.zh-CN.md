# Luna 0.3 核心冻结边界

> 状态：候选契约
> 范围：Luna 0.3 可观察核心行为，不含下列明确开放区域

本文区分“契约冻结”与“源码冻结”。只要不改变可观察契约，冻结区域仍可接受正确性、
安全、诊断、性能修复和内部重构。若冻结的协议、产物或 ABI 发生破坏性变化，必须走
对应的版本升级。

只有一个精确 Luna 源码提交通过完整非硬件测试和全部受支持平台 CI 后，本候选才成为
实际核心冻结。随后 Toolchains 与 Lunax 0.2.0 的发布证据只能使用该精确提交。

## 候选冻结表面

- 除 Slot/Fragment 声明、组合、控制流、retention 与 discovery 外的核心语法和语义；
- 名义类型、record、enum、`Result`、泛型、trait、类型身份、上下文数值字面量，以及
  Value/Meta/Compiler 域分离；
- 所有权、借用、cleanup、Drop、Core `Rc`/`Arc`，以及拥有型 Copy/Affine/Linear
  closure capture；
- package/module/workspace 身份、manifest kind、依赖锁定，以及已记录的 `check`、
  `analyze`、`run`、`build` 命令契约；
- C FFI 子集和 Runtime host、allocator、console、filesystem、foreign resource、
  executable-memory service ABI v1；
- diagnostic JSONL v1，以及 analysis JSONL v1 的 envelope、顺序、位置和 summary 规则；
- Moon/Native container header、proof record、稳定身份字段、完整性规则，以及非
  Slot/Fragment declaration payload；
- 仅限当前受支持 64 位目标模型的 `usize`/`isize`。

非 Slot/Fragment 声明的编译期 metadata/catalog/query 行为进入冻结。query-only value
仍必须擦除，不得嵌入 Value-domain 类型，也不得跨普通运行时边界。

## 明确开放或排除

- Slot/Fragment 的全部拼写、语义、嵌套/重入、continuation、runtime retention、
  discovery 与 descriptor 含义；
- mutable slice 最终源码拼写，以及未来 `Vec`、拥有型 `String`、`Read`/`Write`、
  formatting 和高层 I/O API；
- borrowed closure capture 与公开的跨函数 Iterator ABI；
- 非 64 位 `usize`/`isize` 策略；
- 更广硬件 GPU 支持、并发/异步表面和全工具链性能预算。

现有 descriptor 数字 Fragment=2、Slot=8 及其 record layout 保留且不得复用，但这不
冻结 Slot/Fragment 语义。不兼容的含义必须使用新的 ABI/container 版本或显式协商的
capability。

## 协议扩展规则

`luna.analysis` version 1 冻结 JSONL envelope 与 record 结构，但 `symbol_kind` 等字符串
词汇属于开放枚举。consumer 应尽可能保留未知值，否则降级成 unknown/generic symbol，
不得令整个 stream 失败。新增必填字段或改变既有含义必须提升协议版本。

## 候选门禁

记录 candidate commit 前必须满足：

1. 数值 token 无异常解析；上下文整数字面量完成范围检查；不可表示的 value layout 被拒绝；
2. 每个 Value-domain constructor 拒绝具体 Meta/Compiler-domain 实参，MoonIR verifier
   再次检查同一不变量；
3. 语义负例矩阵、完整非硬件 CTest，以及 Linux、macOS、Windows CI 全部通过；
4. Toolchains/Lunax 使用精确 candidate commit，而不是分支名或未来 tag。
