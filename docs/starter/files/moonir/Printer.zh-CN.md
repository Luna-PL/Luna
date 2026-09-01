# src/moonir/Printer.cpp

Printer.h 的实现：把 Module / TypeRecord / declaration / metadata 打印成人读文本。

## 这个文件做什么

print() 分块输出：

- 顶部：模块名与版本、active features（runtime、kernel、kernel_runtime_reserved）、sourceModule、packageUses；
- 类型块：每个 TypeRecord 一行（id、shape、abi_layout、domain、identity、sysmeta 资源契约、abi_size/abi_align）；
- metadata schema；declarationTable（decl 行 + sysmeta 子行 + metadata attach）；
- 声明列表：function / fragment 分别打印名字(参数…) -> 返回类型，并带 kernel / deferred_recipe / generic_recipe / instantiation 标记。
printCostReport 逐行打印 module->costs。

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| Printer::print | 主体文本打印（人读格式、行对齐）。 |
| Printer::str | 经 std::ostringstream 返回字符串。 |
| Printer::printCostReport | 打印成本清单，空则提示无成本。 |

## 与周边文件·阶段的关系

- 读取 MoonIR.h 的各种记录结构，调用其 *Name() 映射与 findType。
- 阶段：诊断与测试链，不影响 IR。

## 延伸阅读
- 接口：src/moonir/Printer.h。
- 结构：src/moonir/MoonIR.h。


---

---
title: Printer 接口：把 MoonIR Module 文本化
file: src/moonir/Printer.h
namespace: moon
阶段: 诊断 / 测试薄层
---

# src/moonir/Printer.h

声明 Printer：把 Module 打印成面向人的文本，并可产出成本报告字符串。

## 这个文件做什么

一个只读打印工具：print(Module, ostream)、str()（返回 std::string）、printCostReport(Module, ostream)。供调试、测试夹具与诊断输出使用；不修改 Module。

## 关键结构体·类

| 类 | 目的 |
| --- | --- |
| class Printer | 三个公开方法。 |

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| void print(const Module&, std::ostream&) | 打印模块主体（类型、声明、函数等）。 |
| std::string str(const Module&) | 用 stringstream 包装 print 返回字符串。 |
| void printCostReport(const Module&, std::ostream&) | 打印 module->costs 列表。 |

## 与周边文件·阶段的关系

- 读取 MoonIR.h 里的 Module/TypeRecord/DeclarationRecord 结构。
- 阶段：诊断、测试、工具链的薄层，不影响 IR 语义。

## 延伸阅读
- 实现：src/moonir/Printer.cpp。
- 结构：src/moonir/MoonIR.h。


---

---
title: Sealer 实现：原子密封函数体
file: src/moonir/Sealer.cpp
namespace: moon
阶段: MoonIR 密封
---
