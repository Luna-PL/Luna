> Document category: implementation note / tutorial
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# 术语表：C/C++ 概念 → Luna/编译术语

阅读源码时遇到陌生词，先来这查。每词：原名、中文意思、一句话解释、与本仓库源码的落点。

## A. C/C++ 已有概念 → 编译器世界里的对应物

| 术语 | C/C++ 里你已懂的 | 编译原理/Luna 里的含义 | 源码落点 |
|---|---|---|---|
| AST | 语法树 | 把源码解析成树状结构 | src/parser/AST.h |
| Token | 单词 | 词法单元（关键字/标识符/数字/运算符） | src/lexer/Token.h |
| CFG | if/while 的分支跳转 | 控制流图：基本块 + 边 | src/moonir/ControlFlowBuilder |
| SSA | 每个值只赋值一次 const | 静态单赋值 | codegen / 前置课A |
| lowering | 无 | 把高层表示翻成低层表示 | src/moonir/Lowering.cpp |
| 指令 inst | 一条语句 | 一条可执行操作 | src/moonir/MoonIR.h |
| IR | ? | 中间表示（MoonIR / LLVM） | src/moonir、LLVM |
| sealed | 无 | 把函数体转成规范 CFG | src/moonir/Sealer.cpp |
| 验证 verify | assert | 检查 IR 完整性 | src/moonir/Verifier.cpp |
| 优化 | -O2 | 变换 IR 使其更快/更小 | src/moonir/Optimizer.cpp |
| ABI | 无 | 二进制层的调用/布局契约 | src/runtime/RuntimeABI.h |
| 对齐 | alignas | 地址需被整数整除 | src/core/TypeLayout.h |
| tag | union 的判别 | sum 类型判别码 | inline ADT tag |
| 句柄 | 指针/引用 | 轻量引用真实数据的值 | src/core/TypeLayout.h |
| ownership | 所有权 | 谁负责释放；Relation+Usage | src/core/Ownership.h |
| RAII | RAII | 析构时清理；Luna 用编译期 cleanup | codegen Cleanup |
| nominal | 命名类型 | 名义类型，同名才相同 | src/core/TypeIdentity.h |
| structural | 结构类型 | 按形状比较 | src/core/TypeRelations.h |
| 实例化 | template/specialization | 泛型实例化 | src/instantiation |
| trait | concept/接口 | 一组能力约定 | src/sema/TraitChecker |
| 编译期求值 | constexpr | 纯编译期求值 | src/sema/CompileTimeEvaluator |

## B. Luna 特有概念

| 术语 | 含义 | 落点 |
|---|---|---|
| MoonIR | Luna 中间表示（约1.5万行） | src/moonir |
| sealed | 密封：转成规范 CFG | src/moonir/Sealer.cpp |
| place | 可写的内存位置（变量/字段） | MoonIR PlaceProjection |
| Affine / Linear | move-only：用一次，或用一次必须销毁 | src/core/Ownership.h |
| borrow | 借用（共享/可变） | src/sema/OwnershipChecker |
| selector | 编译期选择器 | src/selector |
| Moon Container | 确定性序列化/加载的模块容器 | src/moonir/Container |
| host service | 宿主能力（I/O/allocator） | src/runtime/ApplicationHostServices |

## C. 怎么用

遇到不认识词 → 查本表 + 查相应导读 → 回到源码。本表不承诺语言语义，语言语义永远以 docs/reference/ 为准。
