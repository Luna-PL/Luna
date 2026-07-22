# Luna Documentation Roadmap

## 第一阶段：冻结架构

1. 审阅 `Luna_Language_Architecture_Outline.md`。
2. 修正层级、术语和已确定原则。
3. 将仍有争议的内容移入“尚待讨论的问题”。
4. 冻结 `Luna_Glossary.md` 第一版。

## 第二阶段：拆分规范

优先顺序：

1. `metadata.md`
2. `declaration.md`
3. `selector.md`
4. `runtime.md`
5. `dynamic.md`
6. `type_system.md`
7. `constraints.md`
8. `ownership.md`
9. `plugin.md`
10. `abi.md`

## 第三阶段：补实现文档

1. 前端管线。
2. HIR / MIR。
3. Type / Constraint / Trait Solver。
4. Runtime Descriptor。
5. Registry。
6. Runtime Select。
7. Dynamic Reflection。
8. Plugin Load / Unload。
9. ABI 与兼容检查。

## 第四阶段：建立 RFC 流程

每个新设计必须说明：

- 所属层级。
- 静态成本。
- Runtime 成本。
- Dynamic 成本。
- ABI 影响。
- 安全影响。
- 与现有机制的关系。
- 迁移策略。

## 当前建议优先讨论

1. Metadata 正式语法。
2. Declaration Identity。
3. Selector Protocol。
4. Runtime Descriptor 最小字段。
5. Runtime Registry 所有权模型。
6. Dynamic Reflection 的保留等级。
7. Plugin Unload 与对象占用检查。
