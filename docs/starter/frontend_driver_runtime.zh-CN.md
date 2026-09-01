---
Document category: implementation note
Applies to: Luna 0.3.0 development
Status: Implemented Experimental
Normative status: non-normative
---

# Luna 前端 · 驱动器 · 运行时 阅读指南

> 面向学过 C/C++ 的读者；不假设具备 ABI / extern C / 编译原理背景知识。本文以真实源码走读为据，所有符号均来自 src/ 实际文件（未编造）。

## 覆盖范围

| 层 | 目录 | 文件 |
|---|------|------|
| 前端 | src/lexer, src/parser | Token.h, Lexer.h, Lexer.cpp, AST.h, Parser.h, Parser.cpp |
| 包装载 | src/package | Package.h, Package.cpp, PackageManager.h, PackageManager.cpp |
| 驱动器 | src/driver | Driver, CommandLine, CompilerPipeline, AotLinker, Repl (各含 .h/.cpp) |
| 运行时 | src/runtime | RuntimeABI.h, Runtime.h, Runtime.cpp, ApplicationHostServices.h/.cpp |

建议阅读顺序：前端 -> driver -> runtime。每部分给：定位 / 关键结构 / 文件责任表 / 真实代码走读 / 面向 C++ 读者的新概念。

# 第一部分：前端（Lexer / Parser）

## 1.1 定位

前端把源文本变成带源码位置的语法树（AST）。分两级：
- 词法（Lexer）：字符流 -> `std::vector<Token>`；
- 语法（Parser）：Token 流 -> `Program`。

用 C++ 类比：`ASTNode` 类似一个带虚析构的基类，所有节点继承它；每个节点自带 `sourcePath/line/col`。

## 1.2 关键结构：Token

`src/lexer/Token.h` 定义：
- `TokenKind`：一个 enum，把所有关键字、内置类型（TyI32/TyF32 等）、运算符、分隔符、字面量与标识符、以及边界 `EndOfFile/Error` 都编号。
- `struct Token { TokenKind kind; std::string lexeme; int line; int col; }`
- `KEYWORDS`：unordered_map<string, TokenKind>，把原文映射到枚举（如 fn->TokenKind::Fn）。
- `tokenKindName`：反查字符串，供报错/打印。

## 1.3 关键结构：Lexer 入口

`src/lexer/Lexer.h/.cpp`：
```cpp
class Lexer {
public:
  explicit Lexer(std::string source, std::string sourceName = "<input>");
  std::vector<Token> tokenize();
  const std::vector<diagnostic::Diagnostic>& errors() const;
};
```

真实代码走读（Lexer.cpp 的 tokenize 主循环）：

```cpp
std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (!isAtEnd()) {
        mStartLine = mLine; mStartCol = mCol;
        char c = peek();
        if (std::isalpha(c) || c == '_') tokens.push_back(readIdentifierOrKeyword());
        else if (std::isdigit(c))        tokens.push_back(readNumber());
        else if (c == '"')               tokens.push_back(readString());
        else                             tokens.push_back(readOperator());
        skipWhitespace();
    }
    tokens.emplace_back(TokenKind::EndOfFile, "", mLine, mCol);
    return tokens;
}
```

要点：`skipWhitespace` 跳过空白/注释 URL，`match(c)` 用于匹配 `:` `=` 等组合。错误写入 `mErrors`，并产生 Error token。

## 1.4 关键结构：Parser 入口

`src/parser/Parser.h` 的公开入口：
```cpp
class Parser {
  std::unique_ptr<Program> parse();
  // ...
};
```
真实代码走读（parse 主体，节选）：
```cpp
std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>();
    if (check(TokenKind::Package)) { parsePackageHeader(program.get()); program->isPackage = true; }
    while (!isAtEnd()) {
        if (auto decl = parseDeclaration()) program->declarations.push_back(std::move(decl));
    }
    return program;
}
```

## 1.5 AST 节点层次（src/parser/AST.h）

- 根：Program（含 packageName、modulePath、packageUses、hostImports、declarations）；
- 声明：Decl（FunctionDecl / FragmentDecl / StructDecl / EnumDecl / TraitDecl / ImplDecl / MetaDecl / ConstraintDecl）；
- 语句：Stmt（Let / Block / Return / Expr / If / Match / While / For / SlotDecl / Resume / Abort / Apply）；
- 表达式：Expr（字面量 / Binary / Unary / Call / Field / Index / Block / Lambda…）；
- 类型：TypeAST（Named / Ref / Linear / Affine / Function / Record）。

## 1.6 文件责任表（前端）

| 文件 | 责任 |
|------|------|
| src/lexer/Token.h | Token、TokenKind、KEYWORDS、tokenKindName |
| src/lexer/Lexer.h | Lexer 类：tokenize()、errors() |
| src/lexer/Lexer.cpp | 词法实现（跳空白注释、读标识符/数字/字符串/运算符） |
| src/parser/AST.h | 全部 AST 节点定义 |
| src/parser/Parser.h | Parser 类、parse() 声明 |
| src/parser/Parser.cpp | 递归下降解析实现 |

## 1.7 面向 C++ 读者的新概念（前端）

- Token 就是“带类型的词素”；AST 用 unique_ptr 表达层级，无所有权歧义。


---

# 第二部分：driver（驱动器）

## 2.1 定位

driver 是命令行入口与总编排者：解析 CLI 参数，创建 CompilerPipeline，并把命令分发到 JIT（run）、REPL、AOT（build）等。

## 2.2 关键结构
- `Driver.h/.cpp`：`int run(int argc, char* argv[])` 是入口。
- `CommandLine.h/.cpp`：`parseCommandLine` 解析出 `CommandLineOptions`、CommandLineParseResult；含 MessageFormat(Human/Json) 与 ArtifactTarget(Native/Moon/Cffi)。

## 2.3 real 走读：Driver::run 如何调用 CompilerPipeline / REPL / AOT

真实代码（节选同构）：
```cpp
int luna::driver::run(int argc, char* argv[]) {
    // 1) 识别子命令
    if (cmd == "repl") return run_repl(std::cin, std::cout, std::cerr);
    auto cli = parse_command_line(argc, argv);   // CommandLine

    // 2) 编译: 源 -> MoonIR
    CompilerPipeline pipeline;
    CompilerPipelineOptions opt{ opts.input, opts.level, opts.reserve, opts.is_build };
    if (!pipeline.compileToMoonIR(opt)) { print(pipeline.errors()); return 1; }
    if (cmd == "check") return 0;               // check 只到前端边界

    // 3) 代码生成
    if (!pipeline.generateCode(std::move(opts.gpuTargets))) return 1;
    auto& cg = pipeline.codeGenerator();

    // 4) 调度
    if (cmd == "run")  return cg.jitRun();                          // JIT
    if (cmd == "build") return AotLinker::build(cg, aotOptions);    // AOT 链接
}
```

要点：
- check：编译到 MoonIR 即成功（前端边界）。
- run：generateCode 后 JIT。
- build：AOT（native/moon/cffi 由 ArtifactTarget 分派）。

## 2.4 CLI 选项（真实符号）

- 优化：-O0 / -O2 / -O3（parseCommandLine 支持 O0/O2/O3）
- 输出：--output/-o；流程出处：--emit-moonir <path>
- 链接：--link <lib>；GPU：--gpu-target sim|cuda|rocm
- 诊断格式：--message-format json；分析协议：--overlay（analyze 用）

## 2.5 文件责任表（driver）

| 文件 | 责任 |
|------|------|
| Driver | run() 入口、命令分发，调用 CompilerPipeline/Repl/AOT |
| CommandLine | 参数解析、CommandLineOptions、MessageFormat/ArtifactTarget |
| CompilerPipeline | 源->AST->MoonIR 封装、generateCode |
| AotLinker | AOT 链接（产物可执行/共享库） |
| Repl | 交互运行（JIT） |

## 2.6 面向 C++ 读者的新概念（driver）

- run 是一个巨型命令分发器；所有子命令共享同一条 CompilerPipeline 管线。


---

# 第三部分：runtime（运行时 ABI）

> 关键词：RuntimeABI / ApplicationHostServices。这部分最能体现“C 与 C++ 之分”，全部使用 extern C + 纯 C 结构/函数指针。

## 3.1 定位

src/runtime/ 给 Luna 生成的代码提供运行时服务：内存、控制台、文件系统、GPU、引用计数等，并定义宿主能力/C allocator 接口，作为生成代码与宿主之间的稳定边界。

## 3.2 关键结构：RuntimeABI.h（契约层）

`src/runtime/RuntimeABI.h` 全用 C 结构 + extern C。核心类型：

- `LunaRuntimeStatusV1`：状态码枚举（0=OK，负值表失败）。
- `LunaRuntimeErrorSnapshotV1`：错误快照（domain/code/message_size），统一错误对象。
- `LunaHostCapabilityV1`：能力位掩码（host 提供哪些服务：allocator/console/gpu…）。
- allocator 函数指针表：`LunaAllocateFnV1`、`LunaReallocateFnV1`、`LunaDeallocateFnV1` 等，组成 `LunaAllocatorV1`（C 里的“函数指针表”，即手工 vtable）。

```cpp
typedef void* (*LunaAllocateFnV1)(void* ctx, size_t size, size_t align);
typedef void* (*LunaReallocateFnV1)(void* ctx, void* ptr, size_t old_size, size_t new_size, size_t align);
typedef void  (*LunaDeallocFnV1)(void* ctx, void* p, size_t s, size_t a);
typedef struct LunaAllocatorV1 {
    int version; void* ctx;
    LunaAllocateFnV1 alloc; /* ... */
} LunaAllocatorV1;

int rt_install_host_services_v1(const LunaHostServicesV1*);
```

用 C++ 类比：allocator 是用三个函数指针+context 表达的对象“接口”，相当于把纯虚 allocate/realloc/dealloc 写成函数指针表，从而 ABI 稳定。

## 3.3 ApplicationHostServices（默认宿主 I/O / GPU / allocator）

`src/runtime/ApplicationHostServices.h/.cpp` 提供“默认宿主”：
- 分配器：基于 CRT（malloc/free/realloc）并做对齐；
- 控制台：stdio 的 stdout/stderr 写入；
- 文件系统：用 FILE* 或 POSIX 实现 open/read/write/seek/close；
- GPU：可选（sim / CUDA / ROCm）。
安装/注册入口：`rt_install_host_services_v1`；hosting capability 由宿主提供并声明。

## 3.4 文件责任表（runtime）

| 文件 | 责任 |
|------|------|
| src/runtime/RuntimeABI.h | C 结构/状态码/能力/allocator 函数指针声明（extern C） |
| src/runtime/Runtime.h/.cpp | 默认宿主 + rt_* 设置入口 + 引用计数 + panic |
| src/runtime/ApplicationHostServices.h/.cpp | 默认宿主实现（I/O/GPU/allocator） |

## 3.5 面向 C++ 读者的新概念（runtime）

- extern C + 未命名 struct + 显式版本号，构成稳定 ABI；函数指针表 = 手工接口。
- host 能力位 = 运行前宿主明示能给什么，是轻量的能力协商。
