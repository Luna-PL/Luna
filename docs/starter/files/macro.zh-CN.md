> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/macro/ —— 目录逐文件指南

本指南合并了 src/macro/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: MacroProcessor.cpp
source: src/macro/MacroProcessor.cpp
language: C++ (C++17)
audience: Luna 编译器开发者 / 宏系统实现者
---

# src/macro/MacroProcessor.cpp

MacroProcessor.cpp 是 MacroProcessor::process() 方法的实现。当前版本为无展开的占位实现（no-op stub），用于确立宏处理器在编译器流水线中的所有权和诊断边界，为后续引入真正的宏语法和卫生展开（hygienic expansion）做准备。

## 这个文件做什么

- 实现 MacroProcessor::MacroProcessor(ExpansionLimits) 构造函数：通过成员初始化列表保存展开限制。
- 实现 MacroProcessor::process()：检查输入大小是否超过 maxGeneratedBytes，然后直接输出原始源码，不做任何宏展开。
- 提供唯一的溯源信息：将输入路径作为 provenance 的唯一条目。

## 关键结构体·类·枚举

本文件（.cpp）是纯实现文件，**不定义任何新类型**（无 struct / class / enum / union 声明）。它实现的所有类型均声明于同名头文件：

| 类型 | 声明位置 | 说明（见头文件指南） |
|------|----------|----------------------|
| `ExpansionLimits` | `MacroProcessor.h` | 展开限制：maxGeneratedBytes 等上限配置 |
| `SourceUnit` | `MacroProcessor.h` | 输入源单元：源码文本 + 路径 |
| `Expansion` | `MacroProcessor.h` | 展开结果：输出源码 + provenance 溯源列表 |
| `MacroProcessor` | `MacroProcessor.h` | 宏处理器类，本文件实现其 process() 与构造函数 |

> 若想了解这些类型的内存布局或字段，请直接阅读 `src/macro/MacroProcessor.h` 或对应头文件的 zh-CN 指南。

## 关键函数·方法

### MacroProcessor::MacroProcessor(ExpansionLimits limits)

```cpp
MacroProcessor::MacroProcessor(ExpansionLimits limits)
    : mLimits(limits) {}
```

通过成员初始化列表（member initializer list）保存 ExpansionLimits。limits 通过值传递，然后通过隐式拷贝初始化 mLimits。

### MacroProcessor::process()

```cpp
bool MacroProcessor::process(const SourceUnit& input, Expansion& output,
                             std::vector<diagnostic::Diagnostic>& errors) const {
    output = {};
    if (input.source.size() > mLimits.maxGeneratedBytes) {
        errors.push_back(diagnostic::format(
            "macro", "source exceeds the configured macro expansion byte limit",
            input.path, 0, 0,
            "split the source unit or raise the explicit macro processing limit"));
        return false;
    }

    // Phase A intentionally performs no expansion. Keeping this pass in the
    // live pipeline fixes its ownership and diagnostic boundary before macro
    // syntax and hygiene are introduced.
    output.source = input.source;
    output.provenance.push_back(input.path);
    return true;
}
```

实现逻辑分为两步：

1. 大小限制检查：如果输入源码大小超过 maxGeneratedBytes（默认 16 MiB），则通过 diagnostic::format() 生成一个诊断错误并推入 errors 向量，返回 false。diagnostic::format() 是 diagnostics/Diagnostic.h 中定义的工厂函数，会自动生成错误码（如 macro/MAC9999）。

2. 无展开直通：在 Phase A（当前阶段），宏处理器不做任何语法识别和展开操作，直接将输入源码复制到输出，并将输入路径加入 provenance 溯源列表。这种"无操作但占位"的设计模式在大型编译器项目中常见，用于：
   - 确立宏处理阶段在编译流水线中的固定位置。
   - 锁定错误诊断的边界和格式。
   - 为后续引入真正的宏语法和卫生展开器提供清晰的接口契约。

## 与周边文件·阶段的关系

```
MacroProcessor.cpp -> MacroProcessor.h（类型定义）
                  -> diagnostics/Diagnostic.h（diagnostic::format）
```

- MacroProcessor.h：ExpansionLimits、SourceUnit、Expansion 的类型定义。
- diagnostics/Diagnostic.h：diagnostic::format() 用于生成结构化的诊断错误，支持自动错误码生成和格式化输出。
- package/PackageManager.cpp：在包加载阶段调用 MacroProcessor::process()，对每个收集到的源文件先进行宏展开，再传入词法分析器。

编译器流水线中的位置：

```
输入源文件
  |
MacroProcessor::process()  <- 当前为 no-op 占位
  |
Lexer（词法分析）
  |
Parser（语法分析）
  |
...
```

## 延伸阅读

- MacroProcessor.h：宏处理器接口和类型定义
- diagnostics/Diagnostic.h：diagnostic::format() 和诊断错误系统
- package/PackageManager.h 和 PackageManager.cpp：调用宏处理器的包加载器
- 卫生宏展开（Hygienic Macro Expansion）：Luna 未来计划引入的宏系统，类比 Rust 的 proc_macro 或 Scheme 的 hygienic macro

---

---
title: MacroProcessor.h
source: src/macro/MacroProcessor.h
language: C++ (C++17)
audience: Luna 编译器开发者 / 宏系统与预处理子系统开发者
---

# src/macro/MacroProcessor.h

MacroProcessor.h 声明了宏处理子系统的核心类型：展开限制 ExpansionLimits、源单元 SourceUnit、展开结果 Expansion 以及宏处理器 MacroProcessor。该组件是 Luna 编译器中宏展开（macro expansion）阶段的入口。

## 这个文件做什么

- 定义 ExpansionLimits：配置宏展开的安全限制——最大嵌套深度、最大生成字节数、最大展开次数，防止宏展开导致无限循环或内存爆炸。
- 定义 SourceUnit：封装一个源文件的路径和源码内容，作为宏处理器的输入。
- 定义 Expansion：宏展开的输出，包含展开后的源码和溯源信息（provenance），以及展开次数统计。
- 定义 MacroProcessor：宏处理器类，提供 process() 方法执行完整的宏展开流程。

## 关键结构体·类·枚举

### ExpansionLimits

```cpp
struct ExpansionLimits {
    size_t maxDepth = 64;
    size_t maxGeneratedBytes = 16 * 1024 * 1024;
    size_t maxExpansions = 10000;
};
```

类比 C++ 编译器中 -ftemplate-depth 和 BOOST_PP_LIMIT_MAG 等预处理限制。提供三组安全边界：
- maxDepth：宏展开的递归/嵌套深度上限，防止宏递归展开导致栈溢出。
- maxGeneratedBytes：展开后源码的最大字节数（默认 16 MiB），防止宏展开产生巨量代码（类似 C++ 中模板元编程的"代码膨胀"）。
- maxExpansions：单次处理中宏展开的总次数上限，作为兜底的安全阀。

### SourceUnit

```cpp
struct SourceUnit {
    std::string path;
    std::string source;
};
```

宏处理器的输入单元。path 用于诊断信息和溯源，source 是待展开的源码文本。

### Expansion

```cpp
struct Expansion {
    std::string source;
    std::vector<std::string> provenance;
    size_t expansionCount = 0;
};
```

宏展开的输出。provenance 记录了展开过程中涉及的源文件路径，用于后续诊断和调试。expansionCount 统计展开次数，可用于检查是否接近 ExpansionLimits::maxExpansions 的限制。

### MacroProcessor

```cpp
class MacroProcessor {
public:
    explicit MacroProcessor(ExpansionLimits limits = {});
    bool process(const SourceUnit& input, Expansion& output,
                 std::vector<diagnostic::Diagnostic>& errors) const;
    const ExpansionLimits& limits() const { return mLimits; }
private:
    ExpansionLimits mLimits;
};
```

宏处理器类，采用 RAII 风格，构造函数接受 ExpansionLimits（有默认值）。process() 是核心方法，接收输入源单元，输出展开结果和诊断错误列表。const 限定表示处理过程不修改处理器状态，可重入（reentrant）。

## 关键函数·方法

### MacroProcessor::MacroProcessor(ExpansionLimits limits)

```cpp
explicit MacroProcessor(ExpansionLimits limits = {});
```

构造函数，通过值传递 ExpansionLimits（默认参数使用默认构造的 ExpansionLimits，即 maxDepth=64、maxGeneratedBytes=16MiB、maxExpansions=10000）。explicit 关键字防止隐式转换。

### MacroProcessor::process()

```cpp
bool process(const SourceUnit& input, Expansion& output,
             std::vector<diagnostic::Diagnostic>& errors) const;
```

执行宏展开。接收 input（源单元），通过 output（展开结果）和 errors（诊断列表）输出。返回 bool 表示是否成功。errors 使用 std::vector 而非 std::optional 或 std::error_code，可以容纳多个诊断错误（类似 C++ 编译器中允许多个编译错误的做法）。

## 与周边文件·阶段的关系

```
MacroProcessor.h -> diagnostics/Diagnostic.h（diagnostic::Diagnostic 类型）
```

- diagnostics/Diagnostic.h：定义了 diagnostic::Diagnostic 结构体和 diagnostic::format() 辅助函数，用于生成结构化的诊断错误。
- MacroProcessor.cpp：process() 方法的实现。
- 宏处理阶段在编译器流水线中的位置：词法分析（Lexer）之前。源文件在经过宏展开后，再进入词法分析 -> 语法分析 -> 语义分析等后续阶段。

## 延伸阅读

- MacroProcessor.cpp：process() 方法的实现，当前为无展开的占位实现
- diagnostics/Diagnostic.h：诊断错误格式与报告
- package/PackageManager.h：包加载器，在加载阶段调用 MacroProcessor::process() 对每个源文件进行宏展开

---
