# Luna Architecture Documents

本压缩包包含：

- `Luna_Language_Architecture_Outline.md`：统一架构总纲，包含完整大纲和各章节精简内容。
- `Luna_Design_Principles.md`：最高级设计原则提炼。
- `Luna_Glossary.md`：核心术语表。
- `Luna_Documentation_Roadmap.md`：后续规范拆分和讨论顺序。
- `MoonIR_Metadata_Refactor_RFC.md`：MoonIR、Metadata、Selector、Runtime/Dynamic、泛型与 Kernel 按需生成的已接受 RFC 及实现状态。
- `Static_Meta_Select_Reflection_Constraint_RFC.md`：已实现的静态 Metadata、可遍历 Selector、声明反射与具名 Constraint 的确切职责边界。
- `Type_System_Identity_RFC.md`：Value/Meta/Compiler 类型域、结构形状、名义身份、类型关系与 MoonIR Type Table 迁移设计。
- `Ownership_Affine_Model_RFC.md`：Owned/Borrow 与 Copy/Affine/Linear 的正交模型、Place、控制流合并和 MoonIR cleanup 契约。
- `Error_Result_Panic_RFC.md`：`Result<T, E>`、`?`、abort 型 `panic`、资源清理和非代数效应边界。

建议先直接修改总纲中的错误或争议点，再以总纲为基础拆分正式 Specification。
