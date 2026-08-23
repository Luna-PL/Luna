# Luna 代码生成（Codegen / LLVM 后端）阅读指南

> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative（本文件是“怎么读 codegen 源码”的实现笔记，不定义语言/ABI 契约；契约以 docs/runtime_abi.md 与 docs/reference/type_system.md 为准）
> 代码审计：基于 src/codegen/ 当前源码逐文件核实后撰写

本文面向**学过 C/C++、但完全没接触过 LLVM C++ API** 的读者，帮你把 src/codegen/（约 5000 行）读通：先用 C/C++ 类比讲清 LLVM 的每个核心概念，再落回 Luna 的真实代码。文中出现的类、函数、文件均已逐文件核实，不臆造。

---

## 1) metadata

- Document category: implementation note
- Applies to: Luna 0.3.0 development
- Status: Implemented Experimental
- Normative status: non-normative

---

## 2) 一句话定位

src/codegen/ 把**已经通过验证（verified）的 MoonIR** 翻译成 **LLVM IR**：一个 moon::Module（src/moonir/MoonIR.h）进来，得到一个 llvm::Module（内存里的一整套 IR），再交给两套出口：

- **JIT**：CodeGenerator::jitRun() 用 ORC 的 LLJIT 就地生成机器码并执行入口函数 main；
- **AOT**：CodeGenerator::emitObjectFile() 把 llvm::Module 打印成文本 IR 并写盘，再交由外层 build 流程做系统链接成可执行文件。

也就是说，codegen 是一个“IR 到 IR”的翻译器（moon::Module → llvm::Module）加“IR 到可执行物”的编排器（JIT / object）。寄存器分配、指令选择等低层工作完全交给 LLVM 后端，codegen 自己不做。

---

## 3) 为什么有 CGHelpers：Luna 类型到 llvm::Type 的映射

CGHelpers（CGHelpers.h / CGHelpers.cpp）是 codegen 的“类型词典”：它持有某个 llvm::LLVMContext&，核心方法 toLLVMType(const TypePtr&) 把 Luna 类型对象（其 kind 字段是 TypeKind，定义在 core/TypeSystem.h）映射成对应的 llvm::Type。

类比：llvm::Type 约等于 C 的“类型描述符”（int、double、struct X）——只描述形状、本身不占内存；真正占内存的是“该类型的值或对象”。CGHelpers 就是 Luna 类型到 LLVM 类型的一个形状字典。

各 TypeKind 的对应关系（见 CGHelpers.cpp 中 toLLVMType 的 switch）：

| Luna TypeKind | 生成的 llvm::Type | C/C++ 类比 |
|---|---|---|
| I8 / U8 | i8 | int8_t / uint8_t |
| I16 / U16 | i16 | int16_t |
| I32 / U32 | i32 | int32_t |
| I64 / U64 | i64 | int64_t |
| USize / ISize | i64 | size_t |
| F32 | float | float |
| F64 | double | double |
| Bool | i1 | bool（LLVM 布尔就是 1 位整数） |
| String / CStr / RawPointer / DeviceBuffer / Metadata / MetadataView / DeclarationView / DeclarationRef | 不透明指针 | 句柄 / 裸指针 |
| Iterator | 不透明指针 | 迭代器整体是一个运行时对象 |
| Result | { i1, [N x i64] } (struct) | 布尔 ok 标志 + 按 8 字节字对齐的载荷数组 |
| Enum | { i32, [N x i64] } (struct) | 32 位 tag + 载荷数组 |
| Array | [N x inner] | 定长内联数组 |
| Slice | { ptr, i64 } | 胖指针 {数据指针, 长度} |
| Event | i32 | 事件句柄用整数 |
| Unit / Never | void | 空返回 / 无返回值 |
| Struct | 指针（不透明） | 堆上 object，字段用相对偏移访问 |
| Record | 内联 struct | 值语义 struct，直接做成 LLVM 聚合类型 |
| Function | 指针 | 函数指针（LLVM15+ 不透明指针） |
| Closure | { ptr, env… } | 代码指针 + 捕获环境字段，字段 0 是代码指针 |

其中 Result/Enum 的载荷字数是“取 Ok/Err 两分支尺寸较大者、按 8 字节取整”算出来的。另有两个自由函数 typeSize 与 typeAlignment 返回某类型占多少字节、对齐到几字节，供运行时分配器 rt_alloc 使用；Struct / Record / Closure / Enum / Result 这些复合类型把布局计算交给了 core/TypeLayout.h 的 luna::layout 系列，codegen 只是调用者。

## 4) LLVM 前置速成（面向 C/C++）

以下 8 个概念是读 codegen 前必须建立的心智模型，每个先给 C/C++ 类比，再给真实 API。

### 4.1 llvm::Value 与 llvm::Type —— “值”与“值的类型”

C 里 int x = 5 中，x 是变量、5 是值、int 是类型。LLVM 里运行期出现的几乎都是 llvm::Value*：常数、算出的临时结果、函数指针等，都是 Value 子类；Value::getType() 返回其 llvm::Type*。ConstantInt / ConstantFP / ConstantPointerNull 是“编译期常量”的 Value。Luna 的 generateIntLiteral 就是 llvm::ConstantInt::get(i32Ty, value, true)（第 3 参数 true 表示按有符号整数）。

### 4.2 IRBuilder：在“当前插入点”随手生成指令

一个状态型对象，内部记住“当前 BasicBlock + 游标”。调用 CreateAdd / CreateLoad / CreateBr 这类工厂，它会：把指令插到**当前块的末尾**、返回新结果的 llvm::Value*、并让游标前进。CodeGenerator 的成员 mBuilder 就是它，用 SetInsertPoint(bb) 切换当前块。C/C++ 类比：一个自动把“语句追加到当前代码段末尾”的源码生成器。

### 4.3 指令（Instruction）

C 的 a + b 在 LLVM 里是 Add、Sub、Mul、SDiv（有符号除）、FAdd（浮点加）等具体指令，由 IRBuilder 的 Create* 工厂按需生成。Luna 的 generateBinary（CodeGeneratorExpressions.cpp）就是看 lhs 类型是否浮点：浮点走 CreateFAdd / 整数走 CreateAdd；比较用 CreateICmp*（整数）或 CreateFCmp*（浮点），都得到 i1。

### 4.4 BasicBlock 与 CFG

BasicBlock（基本块）是一串顺序指令，末尾**必须有且只有一个 terminator**（Branch / CondBr / Ret / Switch / Unreachable），中途不能跳转。C 类比：一个“没有 return/分支的连续片段”；if/while 让执行在块之间跳。

CFG=基本块集合 + 块间转移边。MoonIR 本身就是一个规范 CFG（moon::ControlFlowGraph），codegen 第一大类任务就是把它逐块地“重放”成 LLVM 的 CFG。

### 4.5 Alloca / Load / Store —— 把 SSA 当内存用

- Alloca（AllocaInst）：类似在栈上 malloc 一块定长内存，返回一个指针。codegen 的 createEntryBlockAlloca 就在函数入口块里分配槽。
- “局部变量”怎么表示？Luna 的常规做法是：给每个局部变量一个 alloca（槽），用 store 写入、用 load 读取。这正是 LLVM 文档所称的 memory model（值存内存、SSA 里只有 load 结果）。两个映射 mLocals / mCanonicalLocals 记录“名字 / LocalId → Alloca”。
- 读一个变量 generateIdentifier ≈ CreateLoad(alloca)；写一个变量 generateAssign ≈ CreateStore(rhs, alloca)。

所以 codegen 并不“攒 SSA 值”传说——多数变量都是“有地址的内存槽”。

### 4.6 Load / Store

Load = 从指针地址读出一个值；Store = 把值写进地址。它们在读变量、写变量、类型转换（coerceCallArgument 里的 CreateIntCast / CreateBitCast）等场景大量出现。

### 4.7 GEP（GetElementPtr）

GEP 是 LLVM 里“计算数组 / 结构 / 某个元素的地址”的数学（只算地址，不分配对象）。最常见的两种：

- 数组 [N x T] 的第 i 个元素：CreateInBoundsGEP(arrayTy, dataPtr, {0, i})，第一个 0 是“高维”回到数组本身、第二个 i 才是元素下标；
- struct 的某个 field：CreateStructGEP(structTy, ptr, fieldIdx)，比如 Closure 结构取字段 0（代码指针）；也有手动版本“i8 字节偏移”，用 CreateGEP(i8Ty, base, ConstantInt(offset)) 配合 luna::layout::productFieldOffset 算字段地址。

C/C++ 类比：&arr[i] 与 &object.field。

### 4.8 CFG 收尾：PHI、Switch、Unreachable

- PHI（CreatePHI）：SSA 里“一个值来自多个前驱块时选哪个”的无分支选择。Luna 的 && / || 短路、GPU launch 结尾的 event 合并都用到。
- Switch（CreateSwitch(tag, defaultBB, n)）：按整数 tag 分发到多个块。Luna 用它做 Enum / Result 的 tag 分发，也在 Enum 清理里用。
- Unreachable：告诉 LLVM 这块不可达。GPU 失败路径与 panic 调用之后都会 CreateUnreachable()。

---

## 5) 各 .cpp 文件责任表

| 文件（src/codegen/） | 主要职责 | 值得注意的符号 |
|---|---|---|
| CodeGenerator.h | 引擎类头：成员、状态表、私有方法声明 | class CodeGenerator；generate / jitRun / emitObjectFile；LunaGpuTargetConfig；LunaOptimizationLevel |
| CGHelpers.h/.cpp | Luna → llvm::Type 与 size/align | toLLVMType；typeSize；typeAlignment |
| CodeGenerator.cpp | 共享的小工具 | coerceCallArgument；createEntryAlloca；resolveType；resolveDeclaration；resolveFunction；allocationTypeForExpr；fieldIndex；error |
| CodeGeneratorModule.cpp | 生成主调度 | CodeGenerator::generate（声明函数 → 生成内核 → 生成宿主 → verify → 优化）；declareFunc；emitRuntimeDescriptors |
| CodeGeneratorFunctions.cpp | 单函数体入口 | generateFunctionBody（含 main 的 host 服务安装、GPU 初始化） |
| CodeGeneratorControlFlow.cpp | 把 MoonIR 规范 CFG 重放成 LLVM CFG | generateControlFlowBody（Alloca、边、cleanup、terminator、O3 环调度） |
| CodeGeneratorExpressions.cpp | 所有表达式 → LLVM 值 | generateExpr 分发器 + 各 generateXxx 发射器 |
| CodeGeneratorCleanup.cpp | 所有权释放 / drop | emitOwnedPayloadCleanup；emitResourceContentsCleanup；emitCanonicalCleanup；getOrCreateDropCallback；emitLunaDeallocation；packResultPayload / unpackResultPayload |
| CodeGeneratorIterator.cpp | 迭代器适配器与终端 | buildIteratorPlan；emitIteratorPipeline；generateIteratorTerminal；emitCallableInvocation |
| CodeGeneratorExecution.cpp | 构造 / LLVM 初始化 / JIT / 符号绑定 | jitRun；emitObjectFile；initializeLLVM；bindRuntime 表 |
| CodeGeneratorGpu.cpp | GPU 内核 + launch | emitKernelPTX；emitKernelHSACO；generateLaunch；generateDeviceBufferPointer；lowerDirectDeviceMemoryToGlobal；makeHipModuleBundle |
| CodeGeneratorRangeAnalysis.{h,cpp} | 数组下标区间证明 | knownArrayIndexUpperBound；isProvablySafeArrayIndex |
| CodeGeneratorRuntimeDescriptors.cpp | 运行时描述符 / 注册表 | emitRuntimeDescriptors；moonRuntimeSectionNames；stableRuntimeId |

调用关系：Module.cpp → Functions（generateFunctionBody）→ ControlFlow（generateControlFlowBody）→ Expressions（generateExpr）；Cleanup / Iterator / Gpu 是三个相对独立又互相调用的发射模块。

---

## 6) 函数体生成 generateFunctionBody 与 CFG 映射

### 6.1 入口

generateFunctionBody(FunctionDecl*)（CodeGeneratorFunctions.cpp）：

1. 找到对应 llvm::Function。函数声明在 CodeGeneratorModule.cpp 的 declareFunc 里先行创建，解决引用。
2. 清空“当前函数”的编译状态：mLocals、mLocalTypes、mCanonicalLocals、mArrayDropFlags、mMaterializedIterators、mLocalKnownUpperBound 等全部 reset。
3. 创建 entry BasicBlock，mBuilder->SetInsertPoint(entryBB)。
4. 若是入口函数（名称为 main）：先插入对 rt_install_application_host_services_v1 的调用（JIT/AOT 共用同一入口策略）；若主机启用 kernel，再插入 rt_gpu_initialize、失败路径调用 rt_gpu_report_initialization_error，并按返回类型回一个通用失败值。
5. 核心调用 generateControlFlowBody(*decl->controlFlow, func, entryBB)。
6. 若返回类型是 void 且当前块没有 terminator，最后补一个 CreateRetVoid()。

### 6.2 Alloca：把局部变量变成内存

generateControlFlowBody（CodeGeneratorControlFlow.cpp）在开头给“所有 local”建槽：

- mCanonicalLocals.assign(graph.locals.size(), nullptr)：先为每个 LocalId 留一个槽位；
- 预扫一遍 blocks 中的 LetStmt：凡用 InitAllocationExpr 作为 initializer 的 local，标记为“指针型”（因为它们存放的是 rt_alloc 返回的指针）；
- 再对每个 graph.locals：若 local.kind == Allocation 或 pointerBacked，就用 ptrTy()；否则用 CGHelpers::toLLVMType(该类型)。然后逐个调用 createEntryBlockAlloca，写入 mCanonicalLocals[id]。

函数参数本质也是 local：LocalKind::Parameter 的 local，会把 LLVM 函数的第 i 号参数（func->getArg(i)）store 进对应的 alloca，之后统一“load local”。注意闭包的环境参数到达时是“指向环境结构的指针”，此处要先 load 成环境值再 store（源码注释：closure environment parameter arrives as pointer to the env struct…）。

> 用心记住：MoonIR 的规范 CFG 特意是“local-based 而非 SSA”的（MoonIR.h 中那段注释就是明言）。所以 codegen 给每个 local 配一块 alloca 槽——读就是 load、写就是 store，非常贴近 C 的存储模型，也没有 SSA。掌握了这点，你就不再对 codegen 为何到处查表感到困惑。

### 6.3 表达式生成：generateExpr

generateExpr(Expr*)（CodeGeneratorExpressions.cpp）是一个分发器：用 dynamic_cast 逐个试探 IntLiteralExpr、IdentifierExpr、BinaryExpr…，命中以后调用对应的 generateXxx（可理解为用 RTTI 的手写 visitor）。它覆盖了：

- 字面量：Int / Float / String / Bool / Unit / ArrayLiteral；
- 值访问：Identifier、DynamicSelect、FieldAccess、SliceLength、Index（Index 对下标调用 rt_array_index_or_abort 做越界检查）；
- 算术：Binary（&& 与 || 被降成小型 CFG 短路，其余走 Create*）、Unary；
- 构造：VariantConstruct（构建 {i32, [Nxi64]}）、ResultConstruct、RecordLiteral、InitAllocation、HeapAlloc（调用 rt_alloc）；
- 调用：generateCall（详见下）；
- 所有权：generateTry（按 Result 的 ok/err 分支，err 走错误路径、发射对应 cleanups 后返回错误 Result）、generateAssign、generateMove、generateBorrow、generateDeref、generateAddrOf；
- 闭包：Lambda、EnvLoad、MakeClosure。

generateCall 是最复杂的单个方法：先判断是否是迭代器终端（Fold / ForEach / Count / Collect → 交给 generateIteratorTerminal）；否则优先查内建辅助名；常规“直接调用”用 call->calleeRef 经 resolveFunction 找到 llvm::Function 再 CreateCall；间接调用（Function 指针 / Closure）则用 CreateCall(fnType, fnPtr, args)，若是 Closure 先取 union 字段 0 的代码指针、并传环境首参，回去做“调用约定适配”。

### 6.4 控制流映射：terminator 逐个重放

generateControlFlowBody 是 CFG 的“总引擎”：先给每个 graph.blocks 预创建 LLVM BasicBlock（名字形如 cfg.<id>，解决前向引用），再把 mBuilder 指到该块、逐条执行 operations（LetStmt / FreeStmt / AllocateStmt / ExprStmt / AwaitStmt），最后按 block.terminator 的 kinds 发指令：

| TerminatorKind | LLVM 落点 |
|---|---|
| Jump | CreateBr(target)，转移前先发射此边的 cleanups |
| Branch | CreateCondBr(condition, trueBB, falseBB) |
| Switch | CreateSwitch(tag, default, n)；带 binding 的 case 会建 bridge：先清理、把 payload 解包到局部变量再跳目标 |
| Return | 非 void：算出值 → 发射 exitCleanups → CreateRet；void 则 RetVoid |
| Resume / Abort | 当作一次跳转（同时带资 cleanups） |
| Unreachable | CreateUnreachable() |
| Invalid | 报错 |

边上的 cleanups 由 emitEdge / edgeTarget 生成“边上的清理 bridge 块”。O3 下，若某块是无条件 Jump、目标序号向后、回边又无 cleanups，块内指令 ≥ 24 条且无 call/原子操作 等，则 shouldUnrollCanonicalLatch 成立——用 setCanonicalLoopUnrollCount(latch, 4) 给 latch 挂上 llvm.loop.unroll.count metadata，请求 LLVM 展开 4 次。

---

## 7) Cleanup 生成 与 Iterator 材料化

### 7.1 所有权清理（CodeGeneratorCleanup.cpp）

Luna：拥有权语言。codegen 用一条 CleanupRecord（place=PlaceRef + type + action + kind）描述“何时要释放谁，怎么释”。入口 emitCanonicalCleanup(const moon::CleanupRecord&)：

- 先沿 place.projections（Field / ConstantIndex / DynamicIndex / Dereference）一层层“投影”，走到要清理的具体字段 / 元素 / 解引用后的指针（一路 CreateGEP、Load）；
- 若带 guard（CleanupGuard，一个“共享游标 nextUnread”），则生成“游标 <= elementIndex”的条件分支，本次才跑清理；
- 按 action 分发（ResultDrop / EnumDrop / ArrayDrop / RecordDrop / DeviceRelease / Drop / Deallocate / None）并转到 emitOwnedPayloadCleanup：
  - String / CStr 跳过（字面量是不可变的 global 常量指针、不拥有堆内存，不能 dealloc）；
  - DeviceBuffer → 调用 rt_gpu_free；
  - Array / Record / Result / Enum / Closure → 调用 emitResourceContentsCleanup 递归清理各字段 / 元素；
  - 其余指针型 → resource 走完再 emitLunaCallocation（调用 rt_dealloc、带 size+align）；
- Result 的清理会按 is_ok 分叉（建 .ok / .err 两个块）；Enum 的清理按 tag 建变体分派块（invalid tag 分支会 panic）；
- 另有 getOrCreateDropCallback(type)：为每个类型做一个 InternalLinkage 的回调 __luna_drop_callback_<hash>，配合类型擦除场景。

### 7.2 Iterator 材料化（CodeGeneratorIterator.cpp）

Luna 的迭代器是惰性的：map / filter / take 是一串“适配器”，直到**终端**（for / fold / for_each / count / collect）才真正消耗元素：

- buildIteratorPlan：把表达式解析成一个 IteratorPlan（source、sourceType、itemType、mode、以及 step 列表）；
- emitIteratorPipeline：构建 iter.condition / body / next / exit 四个 LLVM 块。condition 里用比较（index < limit）判断有没有下一个；body 里按 source 求某个 item（Range 模式用 index；数组/切片用 GEP + Load 或返回元素地址，且分 shared/mutable/value 三种），再逐个跑适配器（map 调某个可调用；filter 对 Copy 的元素直接放行、对 move-only 的 rejected 元素要先清理；take 递减剩余计数、耗尽就跳 exit）；然后把 item 交给终端回调；next 里 index++ 再回到 condition；
- generateIteratorTerminal：对 Fold（用一块累加器 alloca + reducer）、ForEach（action）、Count（计数器）、collect（调用对应的 begin / push / finish 协议）分别生成完整循环；
- emitCallableInvocation：统一把“可调用”（可能是裸函数指针也可能是闭包）当成可调用目标来调用，避免把 ABI 烧成 i32-only。

> “材料化”指：迭代器的“食谱”（plan）必须真的生成具体循环、游标 alloca、剩余计数 alloca 等运行时状态才可执行——这正是 mMaterializedIterators（struct MaterializedIterator）保存的东西。

---

## 8) 模块集成：moon::Module → LLVM → 优化 → AOT / JIT

### 8.1 主流程 CodeGenerator::generate(moon::Module*)

CodeGeneratorModule.cpp 里的 generate() 是编排者：

1. 构造函数（CodeGeneratorExecution.cpp）创建 LLVMContext / Module / Builder / Helpers，并调用 initializeLLVM() 为宿主注册各 Target 与 MC/AsmPrinter。
2. declareFunc：给每个函数先建 llvm::Function 声明（当前函数里后续会产生 forward 引用）；跳过 isSelector、不可达内核、非实例泛型；链接可见性取决于“是否 ABI 可见”（package 里 isExport / extern / main），Never 返回类型加 NoReturn 属性；符号名用 linkName（否则 generatedSymbolName，否则函数名）。
3. 若 features.runtime，调用 emitRuntimeDescriptors()（见 8.3）。
4. 阶段 1：先生成**内核函数体**（generateBodies(true)）——这样后续 launch 能嵌入已产出的 PTX / HSACO 代码对象。
5. 按 mGpuTargets 决定是否调用 emitKernelPTX / emitKernelHSACO。
6. 阶段 2：再生成**宿主函数体**（generateBodies(false)）。
7. 对 llvm::Module 运行 verifyModule（用 verifier 检查）；非 O0 时用 LLVM新 PassBuilder（buildPerModuleDefaultPipeline(O2/O3)）对 module 做一整套 pass，优化后再 verify。

### 8.2 两种落地

- **JIT**（Execution.cpp 的 jitRun）：LLJITBuilder().create() → 在主 JITDylib 上注册整张 runtimeSymbols 表（bindRuntime 把 rt_alloc、rt_rc_*、rt_arc_*、rt_panic、rt_console、rt_gpu_* 等所有 luna 运行时 helper 注册为绝对符号；Win32 额外注入 __main no-op）→ addIRModule(ThreadSafeModule(...)) 把 module+context 交给 JIT → 再挂一个 EPCDynamicLibrarySearchGenerator libc / 用户库（Luna 自身的符号已显式绑定，不依赖宿主进程导出表）→ lookup(“main”) → 转成入口函数 toPtr<int()>() → 调用。这个入口用 LLVM_NO_SANITIZE(“function”) 包一层，避免 UBSan 在 ORC 函数的探针踩点。
- **AOT**：emitObjectFile 取主机 triple 设进 llvm::Module（setTargetTriple），然后用 raw_fd_ostream 直接把 module 打印成文本 IR（注释："text IR, avoids bitcode compat issues"）写上 .ll；**不在这里生成 .o**，系统链接由外层 build 流程完成。

因此 run（JIT）与 build（AOT）是两条独立、但共享同一个 mModule 的落地路径——这正是 tests/jit_aot_parity.cmake 能断言“JIT 与 AOT 行为必须一致”的缘由。

### 8.3 运行时描述符（CodeGeneratorRuntimeDescriptors.cpp）

当 features.runtime 时，codegen 会以常量全局数据的形式生成一份“运行时描述符/注册表”：先按平台选 section（Mach-O 用 __DATA,__moon_desc；COFF 用 .moon$D；ELF 用 .moon.runtime），把 declarationTable 里需保留的记录编成声明描述符（__moon_descriptor_<hash>），外加一个 __moon_registry（个数 + 指针数组），放 registry section；最后用 llvm::appendToCompilerUsed 保住（防止被优化掉）。这给未来一个“MoonRuntime loader”直接枚举定位。stableRuntimeId 是 FNV-1a 哈希。

---

## 9) GPU 目标简述

- 内核与普通函数共享同一份“host 侧 LLVM IR”，CPU 模拟器可以直接调用它；真正的设备 code object 是把该函数再 Clone 一份、改调用约定与地址空间。因此 host simulator 恒可用。
- **CUDA/PTX**（emitKernelPTX）：建一个 deviceModule（nvptx64-nvidia-cuda），用 CloneFunctionInto 把源函数 clone 过去，SetCallingConc(PTX_Kernel)，再“把入口处 index 参数的 store”替换为 blockIdx.x*blockDim.x + threadIdx.x（用 llvm.nvvm.read.ptx.sreg.* 系列 intrinsic），加 nvvm.annotations 标 kernel，用自己的 pipeline 优化，最后由 legacy 后端输出 PTX 文本存到 mKernelPTX。
- **ROCm/HSACO**（emitKernelHSACO）：deviceModule（amdgcn-amd-amdhsa），clone 后改成 AMDGPU_KERNEL；参数 / 入口按地址空间（1）转换；host 里的 alloca 放到 private 地址空间（5）并用 addrspacecast 桥接；同样把 index 替换成（amdgcn_workgroup_id_x*256 + amdgcn_workitem_id_x）；做两次优化（一次规范管理、一次在地址空间改写后清理），用 legacy 后端产出 object，再用 LLVM自带的 ld.lld（-shared）链接成 HSACO，最后打成 HIP 能识别的 CLANG_OFFLOAD_BUNDLE（host 空图 + device 图，带 4KiB 对齐）。
- **host launch**（generateLaunch）：按当前后端分三路：模拟路用一个 for 循环 0..threads 逐个直接调用内核 LLVM 函数的普通 call；CUDA / ROCm 分别把参数收成“地址数组”传给 rt_gpu_launch_ptx / rt_gpu_launch_hsaco；最后用 CreatePHI 把三种 event 合并成一个 i32。

GPU 目标作为一个显式配置（LunaGpuTargetConfig：emitPTX、cudaArch=sm_52、emitHSACO、rocmArch=gfx1101），运行时后端由用户设置的环境变量 LUNA_GPU_BACKEND 决定（Runtime.cpp），codegen 不会偷偷改 AOT/JIT 产物。

---

## 10) C++ 读者新概念索引

- 值 vs 类型 → 4.1
- IRBuilder 自动插入 → 4.2
- 指令类别 → 4.3
- BasicBlock / CFG → 4.4
- Alloca 模拟局部变量 → 4.5 与 6.2
- Load / Store → 4.6
- GEP 计算地址 → 4.7
- PHI / Switch / Unreachable → 4.8
- LLVM 的不透明指针 → 4.1（与 3）

一个资深 C++ 读者最容易踩的坑：以为 codegen 在“往 SSA 里攒值”。恰恰相反，Luna 大量使用“每个变量一个 alloca、读写都走内存”，因为 CANONICAL 的 MoonIR 是 local-based 而非 SSA（见 MoonIR.h 注释）。有这个背景，读大部分 codegen 都顺理成章。

---

## 11) 阅读顺序

1. 读本文第 4 节，建立 Value / Type / IRBuilder / BasicBlock / GEP 的心智模型。
2. 读 CGHelpers.cpp — 登录 LLVM 的第一扇门，也最小。
3. 读 CodeGenerator.h — 浏览类成员与方法名。
4. 读 CodeGeneratorModule.cpp — 全局流程骨架。
5. 读 CodeGeneratorFunctions.cpp → CodeGeneratorControlFlow.cpp — 函数体与 CFG 的 Alloca。
6. 读 CodeGeneratorExpressions.cpp 中关键：generateExpr 分发、generateCall、generateIndex、generateTry、generateClosure。
7. 读 CodeGeneratorCleanup.cpp 与 CodeGeneratorIterator.cpp — 所有权、迭代。
8. 读 CodeGeneratorExecution.cpp — JIT / 符号绑定。
9. GPU 按需。

读的各步随时查 src/moonir/MoonIR.h，因为 codegen 的类型一一对应用到了 moon::Module、FunctionDecl、ControlFlowGraph（Local / Terminator / ProjectionType）、TypeSystem。

---

## 12) 测试（tests/jit_aot 系列）

tests/jit_aot 并不是一个目录，而是 tests/ 下几个由 cmake 驱动的脚本 + fixture：

- tests/jit_aot_parity.cmake：对同一 fixture（tests/fixtures/jit_aot_parity.luna）分别用 “luna run”（JIT）与 “luna build”（AOT），比较退出码 + stdout；并检查生成的 .ll 里出现 rt_print_i32 且没有 printf（证明 print 走 Luna 运行时 ABI）。
- tests/jit_aot_extended_parity.cmake：把同一校验套到更易出差异的两个边界：多文件 package（fixtures/packages/exported_package）与使用 -O2 优化的 GPU 模拟程序（examples/heterogeneous.luna）。
- tests/jit_runtime_symbols.cmake：专门验证 JIT 的 ORC 符号表：纯 CPU 程序 LUNA_GPU_BACKEND=invalid 时不弹 GPU 错（pay-only）；能 resolve rt_alloc / rt_dealloc；sim 后端能 resolve 并跑 GPU 系列符号（断言输出含 42）；指定非法后端时退出码 / 错误消息正确。

核心主张：JIT 能跑不算数 —— 必须 JIT 与 AOT 的 stdout / 退出码完全一致、printf 不泄漏、GPU 边界正确，才能真正证明 codegen 的正确性。

---

*本指南基于 Luna 0.3.0 开发分支源码逐文件核实。若有源码演进冲突，以实际代码为准。*