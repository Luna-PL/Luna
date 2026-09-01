---
kind: source-directory-guide
module: selector
source: src/selector
lang: zh-CN
audience: 想了解 Luna 0.3 编译期声明选择与 Symbol Catalog 的 C/C++ 读者
---

# `src/selector/` — 声明选择与 Symbol Catalog

Luna 0.3 只有 `CompileTime` 与 `Runtime` 两种 retention。该目录实现类型化、有限的
Symbol Catalog query，以及编译期 `select` 返回的声明身份验证。它不生成运行时
selector plan；已删除的 0.2 `dynamic select` 也不在这里兼容。

## `Selector.h`

- `Retention`：区分编译期擦除与显式保留 Runtime descriptor。
- `CatalogSymbol`：保存稳定 `SymbolId`/`FamilyId`/`ContractId`/`TypeId`、规范身份、
  类型、retention 与 metadata。
- `SymbolCatalog`/`SymbolSet`：执行按 kind、family、type 和 phase 的查询，并提供
  `select`、metadata filter、稳定排序、`.one()` 与 `.optional()` 终端操作。
- `DeclarationView`/`Engine`：校验普通 selector 返回的声明仍属于输入候选族，
  且精确选中一项。

## `Selector.cpp`

`SymbolCatalog` 构造时一次性验证所有稳定身份和重复 `SymbolId`；之后查询只返回
不可变视图。Runtime phase 查询会排除 `CompileTime` 声明。`.all()` 按 `SymbolId`
字节序排列；`.all::<M>()` 要求每项恰有一个 metadata key，拒绝浮点和重复 key，
从而保证唯一全序。

`Engine::validate` 仅接受候选视图中的单一声明身份，并分别诊断空结果、多结果、
越界候选和无效视图。

## 与周边阶段的关系

语义分析构建 catalog 与 declaration view，编译期求值执行 query/selector，MoonIR 与 Runtime
只消费显式 runtime-retained 声明的 descriptor。运行时切换由 typed EV004 宿主 binding
承担，不经过 `Engine`。
