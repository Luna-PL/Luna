# Luna 仓库文件与职责指南

> 文档类别：实现说明 / 项目规范
> 适用版本：Luna 0.3.0 开发期
> 状态：Internal
> 规范性：规范
> 实现核对：待本次提交确认（2026-07-31）

本文是 Luna 仓库物理目录和文件职责的唯一权威索引。它回答“代码或文档应该放到
哪里”，不重新定义语言语义；语言契约以当前参考文档和 0.3 总体设计为准。
[0.2 Alpha 语义基线](reference/semantic_baseline_0.2.md)只作为冻结的迁移证据保留。

## 1. 使用规则

1. 每个仓库文件必须出现在本文末尾的精确清单中，并继承最近的目录规则；有逐文件
   条目时，以逐文件条目为准。
2. 一个文件只应有一个主要职责。跨越两个以上阶段的编排放入专门 orchestrator，
   不把新阶段继续堆进已有实现文件。
3. `.h` 声明边界和稳定数据形状；`.cpp` 实现该边界。只有确有跨翻译单元复用价值
   的逻辑才能进入头文件。
4. `src/` 不依赖 `tests/`、`examples/` 或 `benchmarks/`。`core` 不依赖 driver、
   parser、sema、MoonIR 或 codegen；后端不得反向调用语义分析。
5. 测试脚本负责断言，fixture 只负责输入。示例负责教学，不作为唯一回归证据。
6. 规范事实只写在一个文档中；README、架构图和专题文档只能摘要并链接。
7. 新增、移动或删除文件必须同步更新本指南；`luna.file-guide-inventory` 会检查
   精确清单，防止未登记文件进入仓库。
8. 面向人的 Markdown 默认使用英文文件名和英文内容；中文使用同名 `.zh-CN.md`
   伙伴文件。源码、fixture、lockfile、许可证和机器配置不复制成翻译版本。

## 2. 依赖方向

```text
main -> driver
driver -> package/parser -> sema -> MoonIR -> codegen
                         \-> selector / instantiation
codegen -> runtime ABI

core / diagnostics 只能被上层消费，不得反向依赖上层
stdlib、examples、benchmarks、tests 是编译器的消费者
```

允许为了数据模型而跨层包含声明，但不得借此执行逆向阶段调用。例如 MoonIR 可以
保存类型身份，不能重新查询 `SemanticAnalyzer`；codegen 可以消费 cleanup
obligation，不能重新推导所有权。

## 3. 目录职责

| 目录 | 主要职责 | 不得承载 |
|---|---|---|
| `.github/workflows/` | CI、Sanitizer、跨平台构建与发布自动化 | 语言语义、只在 CI 才成立的修复 |
| `benchmarks/` | 可复现性能工作负载和运行脚本 | 正确性门禁、无环境说明的性能承诺 |
| `docs/` | 用户文档、实现说明、路线与发布边界 | 重复的权威语言规则 |
| `docs/reference/` | Alpha 规范、语义基线、类型与错误模型 | 教程、路线图、临时状态 |
| `examples/` | 独立、可阅读的语言用例 | 唯一回归证据、测试断言 |
| `examples/full_showcase/` | 完整 workspace/package 集成示例 | 编译器内部测试逻辑 |
| `examples/slot_plugins/` | slot/fragment 插件示例 | Runtime ABI 的权威定义 |
| `src/` | 编译器和嵌入式 Runtime 实现 | 测试数据、教程 |
| `src/core/` | 无前后端依赖的类型、身份、布局、所有权和 sysmeta 模型 | AST、符号表、LLVM |
| `src/diagnostics/` | 跨阶段诊断格式 | 阶段专属语义判断 |
| `src/driver/` | 命令解析、流水线编排、REPL、JIT/AOT 外围 | 词法/语义/codegen 规则本身 |
| `src/lexer/` | token 定义与词法分析 | AST 或类型检查 |
| `src/parser/` | AST 数据结构与语法分析 | 类型推断、LLVM lowering |
| `src/sema/` | 名字、类型、trait、所有权和借用检查 | MoonIR 优化、机器 ABI |
| `src/selector/` | 声明候选选择的独立模型 | parser 或 codegen 编排 |
| `src/instantiation/` | 泛型实例请求、状态和稳定身份 | AST 语义克隆、LLVM emission |
| `src/moonir/` | 唯一后端 IR、lowering、验证、优化和打印 | 源码名字解析、目标机器细节 |
| `src/codegen/` | 已验证 MoonIR 到 LLVM/JIT/AOT 的 lowering | 重新执行语义或所有权推断 |
| `src/runtime/` | Runtime ABI、宿主服务、GPU backend 与插件边界 | 编译期语义 |
| `src/package/` | package/module/workspace/lock 装载 | 远程 registry 的未实现行为 |
| `src/macro/` | 当前宏处理组件边界 | 未实现语法的虚假入口 |
| `stdlib/` | 随发行包安装的 Luna 核心/标准库 workspace | 编译器硬编码语义的重复实现 |
| `tests/` | CTest 驱动、ABI 测试和断言 | 用户教程 |
| `tests/fixtures/` | 正例/负例/package/workspace 输入数据 | CMake 断言或测试编排 |
| `tests/fixtures/packages/` | 多文件 package 装载与诊断输入 | manifest/workspace 测试逻辑 |
| `tests/fixtures/workspaces/` | workspace/lock/package 图输入 | 网络依赖 |
| `tools/` | 开发者辅助命令 | CI 唯一可用的隐式步骤 |

`src/*` 子目录内未单列的未来文件必须遵守该子目录规则。fixture 的 `_invalid`
后缀表示预期拒绝；无该后缀的 `.luna` 文件表示正例、运行时失败用例或由测试脚本
明确说明的特殊输入。fixture 文件名描述其唯一测试主题。

## 4. 根文件与自动化

| 文件 | 主要职责 | 边界 |
|---|---|---|
| `.gitignore` | 排除构建树和生成产物 | 不隐藏应版本化的源码/基线 |
| `CMakeLists.txt` | 声明目标、安装布局和 CTest 注册 | 测试断言进入 `tests/*.cmake` |
| `VERSION` | 完整发行版本字符串的唯一文本源 | 不保存构建号或平台后缀 |
| `CHANGELOG.md` | 用户可观察变化和迁移记录 | 不取代当前参考 |
| `README.md` | 英文项目入口 | 只摘要并链接权威文档 |
| `README.zh-CN.md` | 中文项目入口 | 与英文入口指向同一事实来源 |
| `LICENSE-MIT` | MIT 许可证原文 | 不加入项目说明 |
| `LICENSE-APACHE` | Apache-2.0 许可证原文 | 不加入项目说明 |
| `.github/workflows/linux-ci.yml` | Linux C++17/C++23、严格警告和 Sanitizer 门禁 | 不发布产物 |
| `.github/workflows/macos-ci.yml` | macOS 稳定核心门禁 | 不声明其他 macOS 版本兼容 |
| `.github/workflows/windows-ci.yml` | Windows MSYS2 UCRT64 门禁 | 不代表 MSVC/MSVCRT 支持 |
| `.github/workflows/release.yml` | tag 校验、三平台预编译包、校验和及 prerelease | 不绕过平台测试直接发布 |
| `.github/workflows/release-evidence.yml` | 核对 lock 指定的公开 release 元数据与资产摘要 | 不创建或修改 release |

## 5. 生产源码逐文件职责

### 5.1 入口、driver 与 package

| 文件 | 主要职责 |
|---|---|
| `src/main.cpp` | 最薄进程入口；把 `argc/argv` 交给 driver |
| `src/Version.h` | 编译期版本常量，与 `VERSION` 同步 |
| `src/driver/Driver.h` | driver 顶层 `run` 接口 |
| `src/driver/Driver.cpp` | 命令分派、用户输出和顶层退出状态 |
| `src/driver/CommandLine.h` | CLI 选项和解析结果数据结构 |
| `src/driver/CommandLine.cpp` | `check/run/build` 参数解析和组合约束 |
| `src/driver/CompilerPipeline.h` | 文件/内存源码到 MoonIR/codegen 的编排接口 |
| `src/driver/CompilerPipeline.cpp` | 顺序执行 frontend、MoonIR 与 codegen 阶段 |
| `src/driver/AotLinker.h` | native AOT 链接请求接口 |
| `src/driver/AotLinker.cpp` | 无 shell 拼接地启动外部 linker |
| `src/driver/Repl.h` | 可注入流的 Alpha REPL 接口 |
| `src/driver/Repl.cpp` | REPL 命令契约、声明累计和临时源码包装 |
| `src/package/Package.h` | package/module/workspace 装载结果和 loader 接口 |
| `src/package/Package.cpp` | manifest、lock、源码图装载与 package 合并 |
| `src/package/PackageManager.h` | package 管理组件的公开边界 |
| `src/package/PackageManager.cpp` | package 管理组件实现；不得虚构远程解析 |

### 5.2 core、lexer、parser 与 diagnostics

| 文件 | 主要职责 |
|---|---|
| `src/core/Ownership.h` | relation、usage、contract 和 cleanup action 基础模型 |
| `src/core/SysMeta.h` | 编译器派生 sysmeta facts |
| `src/core/StableIdentity.h` | Type/Symbol/Contract/Shape/ABI layout 的强类型稳定 ID 与域分离 hash |
| `src/core/TypeIdentity.h` | TypeId/ShapeId 域和身份接口 |
| `src/core/TypeSystem.h` | 目标无关 canonical `Type` 数据模型 |
| `src/core/TypeRelations.h` | 类型等价、形状和身份关系接口 |
| `src/core/TypeRelations.cpp` | 类型关系实现 |
| `src/core/TypeLayout.h` | Alpha 固定布局查询接口 |
| `src/core/TypeLayout.cpp` | 类型 size/alignment/layout 实现 |
| `src/diagnostics/Diagnostic.h` | 统一结构化诊断记录、人类渲染、JSONL 序列化和源码位置展示 |
| `src/lexer/Token.h` | token kind 与 token 数据 |
| `src/lexer/Lexer.h` | lexer 接口 |
| `src/lexer/Lexer.cpp` | 源码字符到 token 与词法诊断 |
| `src/parser/AST.h` | 源码 AST、类型 AST 和 typed annotation 容器 |
| `src/parser/Parser.h` | parser 接口和恢复状态 |
| `src/parser/Parser.cpp` | token 到 AST 与语法恢复诊断 |

### 5.3 sema、selector 与 instantiation

| 文件 | 主要职责 |
|---|---|
| `src/sema/TypeSystem.h` | sema 使用的约束求解声明和共享类型入口 |
| `src/sema/TypeSystem.cpp` | 类型字符串、约束和 sema 类型辅助实现 |
| `src/sema/Inference.h` | 局部类型推断数据结构/算法 |
| `src/sema/SymbolTable.h` | scope、symbol kind 和 symbol info 接口 |
| `src/sema/SymbolTable.cpp` | 词法 scope 与名字绑定实现 |
| `src/sema/BodyAnalyzer.h` | 函数体语义分析组件的窄接口 |
| `src/sema/BodyAnalyzer.cpp` | statement、expression、call、iterator 和 device/launch 语义 |
| `src/sema/CompileTimeEvaluator.h` | 编译期求值组件的窄接口 |
| `src/sema/CompileTimeEvaluator.cpp` | const、constraint、selector 与 reflection/sysmeta 编译期求值 |
| `src/sema/ControlAnalyzer.h` | Slot/Fragment/apply 控制分析组件的窄接口 |
| `src/sema/ControlAnalyzer.cpp` | 控制契约、候选作用域和 fragment 路径一致性分析 |
| `src/sema/DeclarationCollector.h` | 声明收集/注册组件的窄接口 |
| `src/sema/DeclarationCollector.cpp` | 声明、metadata、FFI 和初始 trait/impl contract 注册 |
| `src/sema/SemanticAnalysisSupport.h` | Sema 组件共享且无可变状态的纯辅助函数 |
| `src/sema/SemanticAnalyzer.h` | tooling/compiler 使用的稳定语义分析 facade |
| `src/sema/SemanticAnalyzer.cpp` | facade 生命周期和对内部 context 的窄委托 |
| `src/sema/SemanticContext.h` | 当前唯一内部语义状态、catalog 和组件抽取接口 |
| `src/sema/SemanticContext.cpp` | 语义编排、跨组件委托和唯一共享状态服务 |
| `src/sema/SemanticContextAccess.h` | 五个组件各自可访问的状态和服务 capability |
| `src/sema/SemanticContextAccess.cpp` | capability 引用绑定及受限服务转发 |
| `src/sema/TraitChecker.h` | trait coherence/check 阶段接口 |
| `src/sema/TraitChecker.cpp` | trait 定义、impl 与约束一致性检查 |
| `src/sema/TypeResolver.h` | 类型解析、约束与泛型实例化组件的窄接口 |
| `src/sema/TypeResolver.cpp` | 类型 AST 解析、约束求解、推断物化与单态化 |
| `src/sema/OwnershipChecker.h` | ownership/borrow 阶段接口与 place 状态 |
| `src/sema/OwnershipChecker.cpp` | path-sensitive move、borrow、cleanup 检查 |
| `src/selector/Selector.h` | selector 候选、请求和结果模型 |
| `src/selector/Selector.cpp` | 静态/动态候选过滤和唯一选择 |
| `src/instantiation/Instantiator.h` | 泛型实例请求、状态机和 ID 接口 |
| `src/instantiation/Instantiator.cpp` | 稳定实例 key、缓存和状态转换 |
| `src/macro/MacroProcessor.h` | 宏处理组件接口 |
| `src/macro/MacroProcessor.cpp` | 当前宏处理实现；只实现已记录能力 |

`SemanticAnalyzer` 已成为稳定 facade，`SemanticContext` 是拆分期间唯一的内部状态
所有者，`BodyAnalyzer`、`DeclarationCollector`、`ControlAnalyzer`、`TypeResolver`
和 `CompileTimeEvaluator` 是已抽取的粗粒度组件，共享纯辅助函数进入
`SemanticAnalysisSupport`。`SemanticContextAccess` 为每个组件提供独立 capability；
后续组件只能通过对应 capability 依赖 context/core，不能复制 catalog；不得只为减少
行数制造互相访问内部状态的翻译单元。

### 5.4 MoonIR

| 文件 | 主要职责 |
|---|---|
| `src/moonir/MoonIR.h` | MoonIR module、function、instruction、type table 和 cost 模型 |
| `src/moonir/MoonIR.cpp` | MoonIR 数据结构的非内联实现 |
| `src/moonir/ControlFlowBuilder.h` | construction-only structured body 到 canonical CFG 的转换接口 |
| `src/moonir/ControlFlowBuilder.cpp` | 消费暂态 structured body，分配稳定 local/scope/block 并生成唯一 CFG |
| `src/moonir/Container.h` | Moon Container section、资源上限与 reader/writer 接口 |
| `src/moonir/ContainerModel.h` | 八段 canonical model 与完整容器事务 codec 接口 |
| `src/moonir/ContainerModel.cpp` | 固定宽度 payload、递归 CFG code、资源边界、原子解码与 Verifier 接力 |
| `src/moonir/Container.cpp` | M005 binary framing、对齐、SHA-256 与不可信输入验证 |
| `src/moonir/Lowering.h` | typed AST 到 MoonIR 的 lowering 接口 |
| `src/moonir/Lowering.cpp` | 消费 typed facts 生成 MoonIR；不得重新推断语义 |
| `src/moonir/Sealer.h` | concrete executable body 原子封存接口 |
| `src/moonir/Sealer.cpp` | 先构造并验证全部候选 CFG，再一次性替换 structured function body |
| `src/moonir/Verifier.h` | MoonIR verifier 接口 |
| `src/moonir/Verifier.cpp` | 结构、类型、cleanup、control-flow 不变量验证 |
| `src/moonir/Optimizer.h` | 目标无关 MoonIR 优化接口 |
| `src/moonir/Optimizer.cpp` | 只保持 verifier 契约的 MoonIR 变换 |
| `src/moonir/Printer.h` | 文本 MoonIR 与 cost report 输出接口 |
| `src/moonir/Printer.cpp` | 确定性文本序列化和报告 |

### 5.5 codegen

| 文件 | 主要职责 |
|---|---|
| `src/codegen/CodeGenerator.h` | codegen façade、共享 lowering 状态和子过程声明 |
| `src/codegen/CodeGenerator.cpp` | façade 初始化和小型共用实现 |
| `src/codegen/CodeGeneratorModule.cpp` | module 级声明收集、host/kernel 编排与验证 |
| `src/codegen/CodeGeneratorFunctions.cpp` | canonical-only 单函数入口、状态与隐式返回 lowering |
| `src/codegen/CodeGeneratorExpressions.cpp` | 普通 expression/value lowering |
| `src/codegen/CodeGeneratorCleanup.cpp` | cleanup、ADT payload、共享/数组资源释放 |
| `src/codegen/CodeGeneratorControlFlow.cpp` | canonical typed-local CFG 的 LLVM block/local/terminator lowering |
| `src/codegen/CodeGeneratorExecution.cpp` | LLVM 生命周期、ORC JIT、AOT IR 输出 |
| `src/codegen/CodeGeneratorGpu.cpp` | GPU target、buffer ABI、launch 与 code object |
| `src/codegen/CodeGeneratorIterator.cpp` | iterator recipe、pipeline、terminal lowering |
| `src/codegen/CodeGeneratorRangeAnalysis.h` | 安全索引范围证明接口 |
| `src/codegen/CodeGeneratorRangeAnalysis.cpp` | statement/expression 共用范围分析 |
| `src/codegen/CodeGeneratorRuntimeDescriptors.cpp` | runtime declaration/metadata descriptor emission |
| `src/codegen/CGHelpers.h` | 无阶段所有权的 LLVM 小型辅助接口 |
| `src/codegen/CGHelpers.cpp` | codegen 辅助实现；不得持有 module 编排状态 |

新增 codegen 文件必须按 lowering concern 拆分，继续共享 `CodeGenerator` 生命周期；
不得创建第二套语义或第二个后端入口。

### 5.6 runtime

| 文件 | 主要职责 |
|---|---|
| `src/runtime/RuntimeABI.h` | versioned C-compatible Runtime ABI v1 |
| `src/runtime/FragmentPluginABI.h` | versioned external fragment plugin ABI |
| `src/runtime/Runtime.h` | 编译器内嵌 Runtime 的 C++ 内部接口 |
| `src/runtime/Runtime.cpp` | allocator/console/error/GPU/plugin Runtime 实现 |

ABI 头只能做向后兼容的版本化扩展。编译器便利 API、C++ 容器或 LLVM 类型不得泄漏
到两个公开 C ABI 头中。

### 5.7 tooling

| 文件 | 主要职责 |
|---|---|
| `src/tooling/AnalysisSnapshot.h` | 可保留 typed AST、package graph、symbol table 与诊断的只读分析快照 |
| `src/tooling/AnalysisSnapshot.cpp` | package/内存 overlay 源码的 Parser、Sema、Trait 与 Ownership 前端编排 |
| `src/tooling/ReferenceIndex.h` | 编译器已解析声明引用的只读索引接口 |
| `src/tooling/ReferenceIndex.cpp` | 语义引用目标到稳定 SymbolId 的映射、排序与去重 |
| `src/tooling/SourceManager.h` | 版本化内存文档与 UTF-16 position 接口 |
| `src/tooling/SourceManager.cpp` | UTF-8 byte offset、行索引和 LSP UTF-16 position 转换 |
| `src/tooling/SymbolIndex.h` | 稳定 SymbolId、声明元数据与只读查询接口 |
| `src/tooling/SymbolIndex.cpp` | typed AST 声明、签名、package/module 身份和源码位置索引 |

## 6. 标准库、文档、示例与基准

### 6.0 双语迁移状态

英文是所有文档的默认入口。已有成对文件（如 `cli.md`/`cli.zh-CN.md`、
`features.md`/`features.zh-CN.md`、`benchmarks.md`/`benchmarks.zh-CN.md`）应保持
同一规则和版本。以下文件目前仍是单语言或中英混写，属于翻译迁移清单，不得被
误称为“已完成双语”:

| 优先级 | 当前文件 | 需要的默认英文/中文伙伴 |
|---|---|---|
| P2 | `docs/getting_started.en.md` | 仅保留短期迁移入口；正式文件为 `getting_started.md` 与 `getting_started.zh-CN.md` |

这张表是诚实的迁移状态，不是对缺失翻译的豁免。翻译完成一组文件后，应删除对应
行，并运行链接、文件库存和文档状态检查。

### 6.1 标准库

| 文件 | 主要职责 |
|---|---|
| `stdlib/luna.workspace` | 随发行版安装的标准库 workspace |
| `stdlib/luna.lock` | 标准库精确本地依赖图 |
| `stdlib/core/luna.package` | `org.luna.core` manifest |
| `stdlib/core/src/resource.luna` | Core `Clone` 资源协议 |
| `stdlib/core/src/shared.luna` | 普通名义 `Rc<T>`/`Arc<T>` 及 Drop/Clone impl |
| `stdlib/core/src/rc.luna` | `Rc::new`/`Rc::clone` 模块表面 |
| `stdlib/core/src/arc.luna` | `Arc::new`/`Arc::clone` 模块表面 |
| `stdlib/core/src/shared_runtime.luna` | 私有 Runtime ABI v1 共享单元 bridge |
| `stdlib/core/src/prelude.luna` | 核心预导入声明 |
| `stdlib/core/src/error.luna` | 核心错误协议 |
| `stdlib/core/src/option.luna` | 核心 Option ADT |
| `stdlib/core/src/iter.luna` | 核心 Iterator/IntoIterator 协议 |
| `stdlib/std/luna.package` | `org.luna.std` manifest |
| `stdlib/std/src/io.luna` | 标准 I/O 表面 |

### 6.2 文档

| 文件 | 主要职责 |
|---|---|
| `docs/file_guide.md` | 本仓库目录/文件职责和精确清单 |
| `docs/architecture.md` | 阶段关系、数据流和关键不变量 |
| `docs/decisions.md` | 已采用设计的压缩理由 |
| `docs/roadmap.md` | 未实现能力和阶段计划 |
| `docs/luna_0.3_design.md` | Luna 0.3 总体设计草案与已确认/待决边界 |
| `docs/luna_0.3_design.zh-CN.md` | Luna 0.3 总体设计草案的中文对应版本 |
| `docs/luna_0.3_evolution_audit.md` | 受总体设计约束的 Luna 0.3 slot/fragment 历史审计 |
| `docs/luna_0.3_evolution_audit.zh-CN.md` | Luna 0.3 历史审计的中文对应版本 |
| `docs/migration_0.2_to_0.3.md` | 0.2 至 0.3 破坏性变化登记和旧编译器迁移 corpus |
| `docs/migration_0.2_to_0.3.zh-CN.md` | 0.2 至 0.3 迁移记录的中文对应版本 |
| `docs/alpha_release.md` | 0.2.1 支持范围、限制和发布门 |
| `docs/alpha_release.zh-CN.md` | Alpha 发布说明的中文对应版本 |
| `docs/features.md` | 英文已实现功能导航 |
| `docs/features.zh-CN.md` | 中文已实现功能导航 |
| `docs/getting_started.en.md` | 英文构建/安装/首次运行教程 |
| `docs/getting_started.md` | 中文构建/安装/首次运行教程 |
| `docs/getting_started.zh-CN.md` | 中文构建/安装/首次运行教程的规范命名 |
| `docs/windows_build.md` | Windows UCRT64 专项构建 |
| `docs/cli.md` | 英文 CLI/REPL 命令参考 |
| `docs/cli.zh-CN.md` | 中文 CLI/REPL 命令参考 |
| `docs/packages.md` | package/module/workspace/lock 当前实现 |
| `docs/standard_library.md` | 标准库职责和表面 |
| `docs/compile_time.md` | constexpr、概念和静态反射 |
| `docs/versioning.md` | metadata selector/version selection |
| `docs/versioning.zh-CN.md` | metadata selector/version selection 的中文对应版本 |
| `docs/fragments.md` | slot/fragment/CPS/plugin 当前边界 |
| `docs/iterators.md` | iterator protocol、recipe 和 ownership |
| `docs/heterogeneous_compute.md` | GPU target/runtime backend/硬件边界 |
| `docs/runtime_abi.md` | Runtime ABI v1 说明 |
| `docs/testing.md` | 测试层级、命令和发布门 |
| `docs/benchmarks.md` | 英文 benchmark 方法和结果边界 |
| `docs/benchmarks.zh-CN.md` | 中文 benchmark 方法和结果边界 |
| `docs/reference/README.md` | 参考文档入口和权威顺序 |
| `docs/reference/documentation_rules.md` | 文档状态、事实类别和维护规则 |
| `docs/reference/semantic_baseline_0.2.md` | 0.2 Alpha 冻结语义基线 |
| `docs/reference/type_system.md` | 规范类型形成、关系和 ownership |
| `docs/reference/builtin_types.md` | 内置/内部类型逐项清单 |
| `docs/reference/error_model.md` | 错误、panic、Result 和清理契约 |

### 6.3 示例与 benchmark

`examples/*.luna` 每个文件只演示其文件名对应的单一语言主题；带 `_invalid` 的文件
演示诊断。`examples/full_showcase/` 是唯一允许组合大部分 Alpha 表面的完整示例，
其 `foundation` 是库 package，`app` 是消费者 package。`examples/slot_plugins/`
只演示插件使用。

`benchmarks/luna_cpu_*.luna` 分别提供命名操作的 Luna CPU 工作负载；
`cpp23_cpu_suite.cpp` 是对照实现，`cpp23_allocation_support.cpp` 通过非 LTO
翻译单元边界保持分配调用可观察；`luna_gpu_vector.luna` 与
`cpp23_hip_vector.cpp` 是 GPU 对照；三个 `run_*.sh` 只负责可复现构建、采样和报告。
`tools/benchmark_heterogeneous.sh` 是异构 benchmark 的开发者入口，不构成 CI 门禁。

## 7. 测试文件规则

| 文件类别 | 主要职责 |
|---|---|
| `tests/*.cmake` | 启动编译器/产物、检查退出状态、诊断、IR、ABI 或输出 |
| `tests/runtime_abi_c_compile.c` | 证明公开 Runtime ABI 头可由 C 编译 |
| `tests/runtime_abi_test.cpp` | Runtime ABI v1 行为与兼容性 |
| `tests/runtime_gpu_error_test.cpp` | GPU/runtime 错误快照行为 |
| `tests/fragment_plugin_fixture.cpp` | 测试用外部插件共享库 |
| `tests/fragment_plugin_test.cpp` | 插件 ABI 装载/调用边界 |
| `tests/analysis_protocol.cmake` | `luna.analysis` v1 JSONL envelope、声明记录与 byte span 回归 |
| `tests/analysis_snapshot_test.cpp` | 内存/路径分析、部分失败状态与 frontend 生命周期回归 |
| `tests/source_manager_test.cpp` | 内存文档版本、Unicode 和 CRLF 回归 |
| `tests/fixtures/*.luna` | 单主题源码输入；断言必须留在调用它的 CMake 脚本 |
| `tests/fixtures/repl_session.txt` | REPL 标准输入 transcript |
| `tests/fixtures/packages/**` | 文件顺序、package header、export 和聚合诊断输入 |
| `tests/fixtures/workspaces/**` | workspace/lock/manifest 和依赖图输入 |

顶层测试脚本的测试名就是其职责。`semantic_regressions.cmake` 是语言正负例总表；
其他脚本分别锁定 AOT/JIT、MoonIR、package、runtime、GPU、fragment、iterator、
install 或 release 边界。一个新测试若只需加入现有矩阵，应扩展现有脚本；只有新的
运行环境、产物类型或独立 ABI 边界才新增顶层脚本。

## 8. 精确文件清单

下列清单由 `luna.file-guide-inventory` 校验。每个条目继承上文最近的目录或文件
规则；清单本身只证明覆盖，不重复语义说明。构建树、Git 内部文件和被 `.gitignore`
排除的生成产物不属于清单。

<!-- FILE_INVENTORY_BEGIN -->
- `.github/workflows/linux-ci.yml`
- `.github/workflows/macos-ci.yml`
- `.github/workflows/release-evidence.yml`
- `.github/workflows/release.yml`
- `.github/workflows/windows-ci.yml`
- `.gitignore`
- `CHANGELOG.md`
- `CMakeLists.txt`
- `LICENSE-APACHE`
- `LICENSE-MIT`
- `README.md`
- `README.zh-CN.md`
- `VERSION`
- `benchmarks/cpp23_allocation_support.cpp`
- `benchmarks/cpp23_cpu_suite.cpp`
- `benchmarks/cpp23_hip_vector.cpp`
- `benchmarks/luna_cpu_allocation.luna`
- `benchmarks/luna_cpu_arithmetic.luna`
- `benchmarks/luna_cpu_array.luna`
- `benchmarks/luna_cpu_array_scan.luna`
- `benchmarks/luna_cpu_bitmix.luna`
- `benchmarks/luna_cpu_branch.luna`
- `benchmarks/luna_cpu_calls.luna`
- `benchmarks/luna_cpu_nested.luna`
- `benchmarks/luna_cpu_reduction.luna`
- `benchmarks/luna_gpu_vector.luna`
- `benchmarks/run_basic_benchmark.sh`
- `benchmarks/run_cpu_comparison.sh`
- `benchmarks/run_rocm_cpp23_comparison.sh`
- `docs/alpha_release.md`
- `docs/alpha_release.zh-CN.md`
- `docs/architecture.md`
- `docs/architecture.zh-CN.md`
- `docs/benchmarks.md`
- `docs/benchmarks.zh-CN.md`
- `docs/canonical_cfg_remaining_tasks.md`
- `docs/cli.md`
- `docs/cli.zh-CN.md`
- `docs/compile_time.md`
- `docs/compile_time.zh-CN.md`
- `docs/decisions.md`
- `docs/decisions.zh-CN.md`
- `docs/features.md`
- `docs/features.zh-CN.md`
- `docs/file_guide.md`
- `docs/file_guide.zh-CN.md`
- `docs/fragments.md`
- `docs/fragments.zh-CN.md`
- `docs/getting_started.en.md`
- `docs/getting_started.md`
- `docs/getting_started.zh-CN.md`
- `docs/heterogeneous_compute.md`
- `docs/heterogeneous_compute.zh-CN.md`
- `docs/iterators.md`
- `docs/iterators.zh-CN.md`
- `docs/luna_0.3_design.md`
- `docs/luna_0.3_design.zh-CN.md`
- `docs/luna_0.3_evolution_audit.md`
- `docs/luna_0.3_evolution_audit.zh-CN.md`
- `docs/migration_0.2_to_0.3.md`
- `docs/migration_0.2_to_0.3.zh-CN.md`
- `docs/packages.md`
- `docs/packages.zh-CN.md`
- `docs/reference/README.md`
- `docs/reference/README.zh-CN.md`
- `docs/reference/builtin_types.md`
- `docs/reference/builtin_types.zh-CN.md`
- `docs/reference/documentation_rules.md`
- `docs/reference/documentation_rules.zh-CN.md`
- `docs/reference/error_model.md`
- `docs/reference/error_model.zh-CN.md`
- `docs/reference/semantic_baseline_0.2.md`
- `docs/reference/semantic_baseline_0.2.zh-CN.md`
- `docs/reference/type_system.md`
- `docs/reference/type_system.zh-CN.md`
- `docs/roadmap.md`
- `docs/roadmap.zh-CN.md`
- `docs/runtime_abi.md`
- `docs/runtime_abi.zh-CN.md`
- `docs/standard_library.md`
- `docs/standard_library.zh-CN.md`
- `docs/testing.md`
- `docs/testing.zh-CN.md`
- `docs/versioning.md`
- `docs/versioning.zh-CN.md`
- `docs/windows_build.md`
- `docs/windows_build.zh-CN.md`
- `examples/.gitignore`
- `examples/adt.luna`
- `examples/adt_error.luna`
- `examples/basic.luna`
- `examples/closure.luna`
- `examples/compile_time.luna`
- `examples/dynamic_select.luna`
- `examples/ffi.luna`
- `examples/fragment_multishot_free_invalid.luna`
- `examples/fragment_multishot_invalid.luna`
- `examples/fragments.luna`
- `examples/full_showcase/README.md`
- `examples/full_showcase/app/luna.package`
- `examples/full_showcase/app/src/foreign.luna`
- `examples/full_showcase/app/src/main.luna`
- `examples/full_showcase/foundation/luna.package`
- `examples/full_showcase/foundation/src/algorithms.luna`
- `examples/full_showcase/foundation/src/device.luna`
- `examples/full_showcase/foundation/src/dispatch.luna`
- `examples/full_showcase/foundation/src/effects.luna`
- `examples/full_showcase/foundation/src/model.luna`
- `examples/full_showcase/luna.lock`
- `examples/full_showcase/luna.workspace`
- `examples/generic.luna`
- `examples/heap.luna`
- `examples/heterogeneous`
- `examples/heterogeneous.luna`
- `examples/heterogeneous_inflight_invalid.luna`
- `examples/heterogeneous_move_event.luna`
- `examples/heterogeneous_unawaited_invalid.luna`
- `examples/heterogeneous_versioned.luna`
- `examples/inference.luna`
- `examples/inference_error.luna`
- `examples/meta_select.luna`
- `examples/minimal.luna`
- `examples/operators.luna`
- `examples/print.luna`
- `examples/slot_plugins/README.md`
- `examples/slot_plugins/README.zh-CN.md`
- `examples/slot_plugins/loop_plugins.luna`
- `examples/test.luna`
- `examples/test2.luna`
- `examples/trait_versioned_nominal.luna`
- `examples/trait_versioning.luna`
- `examples/trait_versioning_incomplete_invalid.luna`
- `examples/trait_versioning_invalid.luna`
- `examples/versioning.luna`
- `examples/versioning_invalid.luna`
- `src/Version.h`
- `src/codegen/CGHelpers.cpp`
- `src/codegen/CGHelpers.h`
- `src/codegen/CodeGenerator.cpp`
- `src/codegen/CodeGenerator.h`
- `src/codegen/CodeGeneratorCleanup.cpp`
- `src/codegen/CodeGeneratorControlFlow.cpp`
- `src/codegen/CodeGeneratorExecution.cpp`
- `src/codegen/CodeGeneratorExpressions.cpp`
- `src/codegen/CodeGeneratorFunctions.cpp`
- `src/codegen/CodeGeneratorGpu.cpp`
- `src/codegen/CodeGeneratorIterator.cpp`
- `src/codegen/CodeGeneratorModule.cpp`
- `src/codegen/CodeGeneratorRangeAnalysis.cpp`
- `src/codegen/CodeGeneratorRangeAnalysis.h`
- `src/codegen/CodeGeneratorRuntimeDescriptors.cpp`
- `src/core/Ownership.h`
- `src/core/SysMeta.h`
- `src/core/StableIdentity.h`
- `src/core/TypeIdentity.h`
- `src/core/TypeLayout.cpp`
- `src/core/TypeLayout.h`
- `src/core/TypeRelations.cpp`
- `src/core/TypeRelations.h`
- `src/core/TypeSystem.h`
- `src/diagnostics/Diagnostic.h`
- `src/driver/AotLinker.cpp`
- `src/driver/AotLinker.h`
- `src/driver/CommandLine.cpp`
- `src/driver/CommandLine.h`
- `src/driver/CompilerPipeline.cpp`
- `src/driver/CompilerPipeline.h`
- `src/driver/Driver.cpp`
- `src/driver/Driver.h`
- `src/driver/Repl.cpp`
- `src/driver/Repl.h`
- `src/instantiation/Instantiator.cpp`
- `src/instantiation/Instantiator.h`
- `src/lexer/Lexer.cpp`
- `src/lexer/Lexer.h`
- `src/lexer/Token.h`
- `src/macro/MacroProcessor.cpp`
- `src/macro/MacroProcessor.h`
- `src/main.cpp`
- `src/moonir/ControlFlowBuilder.cpp`
- `src/moonir/ControlFlowBuilder.h`
- `src/moonir/Container.cpp`
- `src/moonir/Container.h`
- `src/moonir/ContainerModel.cpp`
- `src/moonir/ContainerModel.h`
- `src/moonir/Lowering.cpp`
- `src/moonir/Lowering.h`
- `src/moonir/MoonIR.cpp`
- `src/moonir/MoonIR.h`
- `src/moonir/Optimizer.cpp`
- `src/moonir/Optimizer.h`
- `src/moonir/Printer.cpp`
- `src/moonir/Printer.h`
- `src/moonir/Sealer.cpp`
- `src/moonir/Sealer.h`
- `src/moonir/Verifier.cpp`
- `src/moonir/Verifier.h`
- `src/package/Package.cpp`
- `src/package/Package.h`
- `src/package/PackageManager.cpp`
- `src/package/PackageManager.h`
- `src/parser/AST.h`
- `src/parser/Parser.cpp`
- `src/parser/Parser.h`
- `src/runtime/FragmentPluginABI.h`
- `src/runtime/Runtime.cpp`
- `src/runtime/Runtime.h`
- `src/runtime/RuntimeABI.h`
- `src/selector/Selector.cpp`
- `src/selector/Selector.h`
- `src/sema/BodyAnalyzer.cpp`
- `src/sema/BodyAnalyzer.h`
- `src/sema/CompileTimeEvaluator.cpp`
- `src/sema/CompileTimeEvaluator.h`
- `src/sema/ControlAnalyzer.cpp`
- `src/sema/ControlAnalyzer.h`
- `src/sema/DeclarationCollector.cpp`
- `src/sema/DeclarationCollector.h`
- `src/sema/Inference.h`
- `src/sema/OwnershipChecker.cpp`
- `src/sema/OwnershipChecker.h`
- `src/sema/SemanticAnalysisSupport.h`
- `src/sema/SemanticAnalyzer.cpp`
- `src/sema/SemanticAnalyzer.h`
- `src/sema/SemanticContext.cpp`
- `src/sema/SemanticContext.h`
- `src/sema/SemanticContextAccess.cpp`
- `src/sema/SemanticContextAccess.h`
- `src/sema/SymbolTable.cpp`
- `src/sema/SymbolTable.h`
- `src/sema/TraitChecker.cpp`
- `src/sema/TraitChecker.h`
- `src/sema/TypeResolver.cpp`
- `src/sema/TypeResolver.h`
- `src/sema/TypeSystem.cpp`
- `src/sema/TypeSystem.h`
- `src/tooling/SourceManager.cpp`
- `src/tooling/SourceManager.h`
- `src/tooling/AnalysisSnapshot.cpp`
- `src/tooling/AnalysisSnapshot.h`
- `src/tooling/ReferenceIndex.cpp`
- `src/tooling/ReferenceIndex.h`
- `src/tooling/SymbolIndex.cpp`
- `src/tooling/SymbolIndex.h`
- `stdlib/core/luna.package`
- `stdlib/core/src/arc.luna`
- `stdlib/core/src/error.luna`
- `stdlib/core/src/iter.luna`
- `stdlib/core/src/option.luna`
- `stdlib/core/src/prelude.luna`
- `stdlib/core/src/rc.luna`
- `stdlib/core/src/resource.luna`
- `stdlib/core/src/shared.luna`
- `stdlib/core/src/shared_runtime.luna`
- `stdlib/luna.lock`
- `stdlib/luna.workspace`
- `stdlib/std/luna.package`
- `stdlib/std/src/io.luna`
- `tests/aot_runtime_boundary.cmake`
- `tests/analysis_protocol.cmake`
- `tests/analysis_snapshot_test.cpp`
- `tests/aot_package_fixture.cmake`
- `tests/control_flow_aot.cmake`
- `tests/core_surface.cmake`
- `tests/diagnostic_protocol.cmake`
- `tests/external_fragment_dispatch.cmake`
- `tests/ffi_aot.cmake`
- `tests/file_guide_inventory.cmake`
- `tests/luna_0_3_design_contract.cmake`
- `tests/migration_0_2_baseline.cmake`
- `tests/shadow_identity.cmake`
- `tests/fixtures/aot_runtime_boundary.luna`
- `tests/fixtures/apply_contract_checked_eagerly_invalid.luna`
- `tests/fixtures/anonymous_record_duplicate_invalid.luna`
- `tests/fixtures/anonymous_record_owned_field.luna`
- `tests/fixtures/anonymous_record_partial_move_invalid.luna`
- `tests/fixtures/anonymous_records.luna`
- `tests/fixtures/array_move_element_invalid.luna`
- `tests/fixtures/comparison_non_numeric_invalid.luna`
- `tests/fixtures/comparison_operators.luna`
- `tests/fixtures/concept_not_satisfied_invalid.luna`
- `tests/fixtures/concepts.luna`
- `tests/fixtures/constexpr_nonconstant.luna`
- `tests/fixtures/context_abort_after_resume_invalid.luna`
- `tests/fixtures/context_abort_leaks_local_invalid.luna`
- `tests/fixtures/context_abort_ownership_mismatch_invalid.luna`
- `tests/fixtures/context_abort_preserves_outer_resource.luna`
- `tests/fixtures/context_continuation_return_invalid.luna`
- `tests/fixtures/context_continuation_return_valid.luna`
- `tests/fixtures/context_linear_guard.luna`
- `tests/fixtures/context_missing_control_invalid.luna`
- `tests/fixtures/context_partial_resume_invalid.luna`
- `tests/fixtures/context_return_ends_fragment_valid.luna`
- `tests/fixtures/core_surface_app/luna.package`
- `tests/fixtures/core_surface_app/src/main.luna`
- `tests/fixtures/drop_intrinsic.luna`
- `tests/fixtures/dynamic_apply_static_slot_invalid.luna`
- `tests/fixtures/dynamic_candidate_contract_mismatch_invalid.luna`
- `tests/fixtures/dynamic_fragments.luna`
- `tests/fixtures/dynamic_fragments_many.luna`
- `tests/fixtures/dynamic_fragments_many_capture_invalid.luna`
- `tests/fixtures/enum_match.luna`
- `tests/fixtures/enum_match_arity_invalid.luna`
- `tests/fixtures/enum_match_duplicate_invalid.luna`
- `tests/fixtures/enum_match_non_exhaustive_invalid.luna`
- `tests/fixtures/enum_match_resource.luna`
- `tests/fixtures/external_fragment_dispatch.luna`
- `tests/fixtures/ffi_generic_invalid.luna`
- `tests/fixtures/ffi_owning_return.luna`
- `tests/fixtures/ffi_owning_return_ignored_invalid.luna`
- `tests/fixtures/ffi_owning_return_type_invalid.luna`
- `tests/fixtures/ffi_result_boundary_invalid.luna`
- `tests/fixtures/ffi_unsupported_abi_invalid.luna`
- `tests/fixtures/ffi_unsupported_type_invalid.luna`
- `tests/fixtures/fragment_contracts.luna`
- `tests/fixtures/generic_argument_count_invalid.luna`
- `tests/fixtures/generic_body_cloning.luna`
- `tests/fixtures/generic_instance_reuse.luna`
- `tests/fixtures/heterogeneous_bulk_transfer.luna`
- `tests/fixtures/heterogeneous_bulk_transfer_invalid.luna`
- `tests/fixtures/interceptor_resume_invalid.luna`
- `tests/fixtures/invalid_export.luna`
- `tests/fixtures/inline_where_not_satisfied_invalid.luna`
- `tests/fixtures/iterator_count_move_only_invalid.luna`
- `tests/fixtures/iterator_closure_callback.luna`
- `tests/fixtures/iterator_filter_move_only_owning_invalid.luna`
- `tests/fixtures/iterator_fold_accumulator_borrow_invalid.luna`
- `tests/fixtures/iterator_fold_accumulator_ignored_invalid.luna`
- `tests/fixtures/iterator_fold_accumulator_linear_invalid.luna`
- `tests/fixtures/iterator_fold_accumulator_move_invalid.luna`
- `tests/fixtures/iterator_fold_move_only_invalid.luna`
- `tests/fixtures/iterator_for_each_move_only_invalid.luna`
- `tests/fixtures/iterator_map_move_only_input_invalid.luna`
- `tests/fixtures/iterator_map_type_invalid.luna`
- `tests/fixtures/iterator_materialized.luna`
- `tests/fixtures/iterator_materialized_borrow_invalid.luna`
- `tests/fixtures/iterator_materialized_linear_source_invalid.luna`
- `tests/fixtures/iterator_materialized_move_only.luna`
- `tests/fixtures/into_iter_diagnostic_clean.luna`
- `tests/fixtures/iterator_materialized_source_use_after_invalid.luna`
- `tests/fixtures/iterator_materialized_twice_invalid.luna`
- `tests/fixtures/iterator_move_only_array.luna`
- `tests/fixtures/iterator_move_only_array_use_after_invalid.luna`
- `tests/fixtures/iterator_mut_borrow_conflict_invalid.luna`
- `tests/fixtures/iterator_pipeline.luna`
- `tests/fixtures/iterator_slice.luna`
- `tests/fixtures/iterator_terminal_use_after_invalid.luna`
- `tests/fixtures/jit_aot_parity.luna`
- `tests/fixtures/kernel_host_effects_invalid.luna`
- `tests/fixtures/kernel_unused.luna`
- `tests/fixtures/lambda_capture_copy.luna`
- `tests/fixtures/lambda_capture_affine_move.luna`
- `tests/fixtures/lambda_capture_affine_use_after_move.luna`
- `tests/fixtures/lambda_capture_affine_invalid.luna`
- `tests/fixtures/lambda_capture_borrowed_invalid.luna`
- `tests/fixtures/lambda_capture_multiple.luna`
- `tests/fixtures/lambda_capture_param_shadow.luna`
- `tests/fixtures/lambda_capture_shadow.luna`
- `tests/fixtures/lambda_linear_parameter_invalid.luna`
- `tests/fixtures/lambda_nested_parameter_capture.luna`
- `tests/fixtures/lambda_nested_partial_capture.luna`
- `tests/fixtures/lambda_nested_transitive_capture.luna`
- `tests/fixtures/lambda_return_closure.luna`
- `tests/fixtures/legacy_fragment_invalid.luna`
- `tests/fixtures/logical_short_circuit.luna`
- `tests/fixtures/missing_return_invalid.luna`
- `tests/fixtures/named_record_construction.luna`
- `tests/fixtures/named_record_missing_field_invalid.luna`
- `tests/fixtures/named_record_unknown_field_invalid.luna`
- `tests/fixtures/never_return_value_invalid.luna`
- `tests/fixtures/never_type.luna`
- `tests/fixtures/nominal_modifier_invalid.luna`
- `tests/fixtures/optimization_constant_fold.luna`
- `tests/fixtures/ownership_affine_drop.luna`
- `tests/fixtures/ownership_affine_requires_move_invalid.luna`
- `tests/fixtures/ownership_all_return_paths_consume.luna`
- `tests/fixtures/ownership_disjoint_field_borrows.luna`
- `tests/fixtures/ownership_heap_parameter_borrow.luna`
- `tests/fixtures/ownership_if_all_paths_consume.luna`
- `tests/fixtures/ownership_if_partial_consume_invalid.luna`
- `tests/fixtures/ownership_loop_consumes_outer_invalid.luna`
- `tests/fixtures/ownership_loop_local_resource_valid.luna`
- `tests/fixtures/ownership_overlapping_field_borrows_invalid.luna`
- `tests/fixtures/ownership_partial_move_branch_invalid.luna`
- `tests/fixtures/ownership_partial_move_invalid.luna`
- `tests/fixtures/ownership_return_cleanup.luna`
- `tests/fixtures/ownership_return_path_leaks_invalid.luna`
- `tests/fixtures/ownership_return_path_valid.luna`
- `tests/fixtures/ownership_unreachable_after_return.luna`
- `tests/fixtures/post_let_usage_invalid.luna`
- `tests/fixtures/package_using_missing_alias_invalid.luna`
- `tests/fixtures/cffi_consumer.c`
- `tests/fixtures/packages/alias_collision/01_first.luna`
- `tests/fixtures/packages/alias_collision/02_second.luna`
- `tests/fixtures/packages/cffi_library/luna.package`
- `tests/fixtures/packages/cffi_library/src/api.luna`
- `tests/fixtures/packages/cffi_no_exports/luna.package`
- `tests/fixtures/packages/cffi_no_exports/src/api.luna`
- `tests/fixtures/packages/cffi_typed_export/luna.package`
- `tests/fixtures/packages/cffi_typed_export/src/api.luna`
- `tests/fixtures/packages/duplicate_export/01_shared.luna`
- `tests/fixtures/packages/duplicate_export/02_shared.luna`
- `tests/fixtures/packages/duplicate_export/03_main.luna`
- `tests/fixtures/packages/duplicate_version/01_greet.luna`
- `tests/fixtures/packages/duplicate_version/02_greet.luna`
- `tests/fixtures/packages/duplicate_version/03_main.luna`
- `tests/fixtures/packages/exported_package/01_math.luna`
- `tests/fixtures/packages/exported_package/02_main.luna`
- `tests/fixtures/packages/exported_package/luna.package`
- `tests/fixtures/packages/package_kind_application/luna.package`
- `tests/fixtures/packages/package_kind_application/src/main.luna`
- `tests/fixtures/packages/package_kind_invalid/luna.package`
- `tests/fixtures/packages/package_kind_invalid/src/main.luna`
- `tests/fixtures/packages/mismatched_package/01_first.luna`
- `tests/fixtures/packages/mismatched_package/02_second.luna`
- `tests/fixtures/packages/module_headers/01_math.luna`
- `tests/fixtures/packages/module_headers/02_main.luna`
- `tests/fixtures/packages/method_references/01_ops.luna`
- `tests/fixtures/packages/method_references/02_main.luna`
- `tests/fixtures/packages/multiple_parse_errors/01_first.luna`
- `tests/fixtures/packages/multiple_parse_errors/02_second.luna`
- `tests/fixtures/packages/self_using/01_main.luna`
- `tests/fixtures/panic.luna`
- `tests/fixtures/panic_message_type_invalid.luna`
- `tests/fixtures/parse_missing_binding_name.luna`
- `tests/fixtures/parse_multiple_declarations_invalid.luna`
- `tests/fixtures/rc_arc.luna`
- `tests/fixtures/rc_arc_core_app/luna.package`
- `tests/fixtures/rc_arc_core_app/src/main.luna`
- `tests/fixtures/rc_implicit_copy_invalid.luna`
- `tests/fixtures/recursive_structural_type_invalid.luna`
- `tests/fixtures/record_block_context.luna`
- `tests/fixtures/reflection_index_out_of_range.luna`
- `tests/fixtures/repl_session.txt`
- `tests/fixtures/resource_drop_after_use.luna`
- `tests/fixtures/resource_drop_copy_weaken_invalid.luna`
- `tests/fixtures/resource_drop_signature_invalid.luna`
- `tests/fixtures/resource_generic_drop.luna`
- `tests/fixtures/resource_generic_drop_layout_invalid.luna`
- `tests/fixtures/resource_recursive_named.luna`
- `tests/fixtures/result_ambiguous_constructor_invalid.luna`
- `tests/fixtures/result_basic.luna`
- `tests/fixtures/result_from_borrowed_source_invalid.luna`
- `tests/fixtures/result_from_conversion.luna`
- `tests/fixtures/result_from_resource.luna`
- `tests/fixtures/result_from_signature_invalid.luna`
- `tests/fixtures/result_match.luna`
- `tests/fixtures/result_match_non_exhaustive_invalid.luna`
- `tests/fixtures/result_match_resource.luna`
- `tests/fixtures/result_payload_abi_invalid.luna`
- `tests/fixtures/result_propagation.luna`
- `tests/fixtures/result_resource_cleanup.luna`
- `tests/fixtures/result_try_error_mismatch_invalid.luna`
- `tests/fixtures/result_try_fragment_invalid.luna`
- `tests/fixtures/result_try_non_result_function_invalid.luna`
- `tests/fixtures/result_try_non_result_invalid.luna`
- `tests/fixtures/result_unwrap_panic.luna`
- `tests/fixtures/safe_array_static_bounds_invalid.luna`
- `tests/fixtures/safe_array_wrong_element_invalid.luna`
- `tests/fixtures/safe_arrays.luna`
- `tests/fixtures/selector_outside_view_invalid.luna`
- `tests/fixtures/selector_user_logic.luna`
- `tests/fixtures/slice_borrow.luna`
- `tests/fixtures/slice_bounds_invalid.luna`
- `tests/fixtures/slice_empty_tail.luna`
- `tests/fixtures/slice_write_source_invalid.luna`
- `tests/fixtures/slot_cardinality_contract_mismatch_invalid.luna`
- `tests/fixtures/slot_fragment_contract_mismatch_invalid.luna`
- `tests/fixtures/slot_missing_contract_invalid.luna`
- `tests/fixtures/static_declaration_reflection.luna`
- `tests/fixtures/structural_enum_equivalence.luna`
- `tests/fixtures/structural_field_order_invalid.luna`
- `tests/fixtures/structural_generic_instance_reuse.luna`
- `tests/fixtures/structural_trait_coherence.luna`
- `tests/fixtures/structural_type_equivalence.luna`
- `tests/fixtures/type_domains_reflection.luna`
- `tests/fixtures/type_relations.luna`
- `tests/fixtures/usage_block_linear_unconsumed_invalid.luna`
- `tests/fixtures/usage_block_loop_unconsumed_invalid.luna`
- `tests/fixtures/usage_block_pattern_unconsumed_invalid.luna`
- `tests/fixtures/usage_blocks.luna`
- `tests/fixtures/usage_contract_weaken_invalid.luna`
- `tests/fixtures/versioned_fragment_contract_change_invalid.luna`
- `tests/fixtures/workspaces/local/app/luna.package`
- `tests/fixtures/workspaces/local/app/src/main.luna`
- `tests/fixtures/workspaces/local/core/luna.package`
- `tests/fixtures/workspaces/local/core/src/alternate.luna`
- `tests/fixtures/workspaces/local/core/src/core.luna`
- `tests/fixtures/workspaces/local/luna.lock`
- `tests/fixtures/workspaces/local/luna.workspace`
- `tests/fragment_lowering_abi.cmake`
- `tests/cffi_artifact.cmake`
- `tests/fragment_plugin_fixture.cpp`
- `tests/fragment_plugin_test.cpp`
- `tests/full_showcase.cmake`
- `tests/gpu_error_boundary_abi.cmake`
- `tests/gpu_target_split.cmake`
- `tests/install_smoke.cmake`
- `tests/iterator_materialized_aot.cmake`
- `tests/iterator_materialized_move_only_aot.cmake`
- `tests/iterator_move_only_aot.cmake`
- `tests/iterator_pipeline_aot.cmake`
- `tests/jit_aot_extended_parity.cmake`
- `tests/jit_aot_parity.cmake`
- `tests/jit_runtime_symbols.cmake`
- `tests/moon_cost_boundaries.cmake`
- `tests/moon_container_test.cpp`
- `tests/moon_container_model_test.cpp`
- `tests/moon_container_cli.cmake`
- `tests/moon_container_fuzz.cpp`
- `tests/moon_container_fuzz.dict`
- `tests/moon_container_fuzz_corpus.py`
- `tests/moon_container_oracle.py`
- `tests/optimization_pipeline.cmake`
- `tests/package_export_abi.cmake`
- `tests/package_manifest_workspace.cmake`
- `tests/package_module_model.cmake`
- `tests/rc_arc_core.cmake`
- `tests/repl_smoke.cmake`
- `tests/resource_drop_aot.cmake`
- `tests/result_error_aot.cmake`
- `tests/result_extended_aot.cmake`
- `tests/return_cleanup_abi.cmake`
- `tests/rocm_isa_abi.cmake`
- `tests/rocm_smoke.cmake`
- `tests/runtime_abi_c_compile.c`
- `tests/runtime_abi_test.cpp`
- `tests/runtime_gpu_error_test.cpp`
- `tests/semantic_regressions.cmake`
- `tests/source_manager_test.cpp`
- `tests/stable_core_parity.cmake`
- `tests/structured_cps_abi.cmake`
- `tools/benchmark_heterogeneous.sh`
- `tools/verify_release_evidence.js`
<!-- FILE_INVENTORY_END -->
