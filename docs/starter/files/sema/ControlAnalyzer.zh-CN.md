---
kind: source-file-guide
module: sema
source: src/sema/ControlAnalyzer.cpp
lang: zh-CN
audience: 想了解 Luna 0.3 slot/fragment 控制分析的 C/C++ 读者
---

# `ControlAnalyzer` — 静态 slot/fragment 控制分析

`ControlAnalyzer` 实现 Luna 0.3 的模块级 slot 声明、fragment 名义 target、词法作用域内的
单 fragment `apply`，以及 continuation once/many 控制路径检查。该组件不再包含
dynamic slot/apply 候选作用域或 plugin dispatch 路径。

主要入口：

- `declareSlot` 构造名义 slot 类型并登记模块级声明。
- `finalizeSlot` 解析并校验 slot 的可选默认 fragment。
- `analyzeSlotDecl` 拒绝不属于 0.3 表面的局部 slot 声明。
- `analyzeSlotInvoke` 解析模块级 slot，检查参数与 continuation，然后选用最内层
  静态 apply 或默认 fragment；无绑定时按 identity fragment 处理。
- `analyzeApply` 解析唯一名义 fragment，只在词法 body 内写入 `mApplyScopes`。
- `analyzeFragmentForSlot` 检查参数 ownership 契约、捕获和 continuation 路径。once
  fragment 不得多次 resume 或在 resume 后 abort；many fragment 不得重放 linear 捕获。

`enterSlotScope`/`exitSlotScope` 只同步静态 slot 与 apply 栈。`selectFragment` 按源声明
key 查找并诊断未知 fragment。

AST 与 ownership checker 消费已解析的名义 fragment 字段，MoonIR lowering 使用同一
静态绑定。运行时 generation 切换位于 typed EV004 宿主边界，不经过该分析器。
