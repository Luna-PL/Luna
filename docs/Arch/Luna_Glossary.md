# Luna Glossary

| 术语 | 精简定义 |
|---|---|
| Compiler | 负责解析、检查、求解、优化与代码生成的实现层。 |
| Core | 提供稳定语言抽象与协议的最小层。 |
| Standard Library | 提供 Version、Selector、IO、集合等通用策略与能力。 |
| Runtime | 提供最小运行时对象表示、Registry、Select 与生命周期。 |
| Dynamic | Runtime 的超集，提供完整运行时反射和修改能力。 |
| Structural Type | 由结构、签名、Constraint 等决定兼容性的类型模型。 |
| Nominal Information | 名字、声明来源等附加身份信息，默认不取代结构兼容性。 |
| Ownership Relation | 值契约中表示 Owned、SharedBorrow 或 MutableBorrow 的所有权维度。 |
| Usage Cardinality | 与所有权正交的 Copy、Affine 或 Linear 使用次数约束。 |
| Affine Value | 至多消费一次、允许丢弃；拥有资源时在退出路径生成清理义务的值。 |
| Linear Value | 必须在每条可达退出路径上恰好消费一次的值。 |
| Place | 根 binding 加字段、索引或解引用投影形成的可独立移动/借用存储位置。 |
| Loan | 对一个 Place 的词法 shared/mutable 借用及其冲突范围。 |
| Cleanup Obligation | MoonIR 中绑定到 Place、action 和 TypeId 的确定性退出清理要求。 |
| Declaration | 可被引用、选择、反射、调用或导出的语言实体。 |
| Declaration Identity | 唯一标识某个具体声明的身份。 |
| Declaration Family | 一组可被共同发现和选择的相关声明。 |
| Candidate Discovery | 根据名字、Module、Metadata 等找到初始候选集合。 |
| Selector | 从候选声明集合中按规则选出声明的策略。 |
| Metadata | 附着于语言实体的结构化数据。 |
| Runtime Metadata | 被保留进 Runtime Descriptor 的 Metadata。 |
| Registered Runtime Metadata | 自动进入 Runtime Registry / Pool 的 Runtime Metadata。 |
| Runtime Object | 具有最小 Runtime Descriptor 的对象。 |
| Dynamic Object | 具有完整 Runtime Reflection 的 Runtime Object。 |
| Runtime Descriptor | Runtime 对象最小可执行、可发现描述。 |
| Runtime Registry | 保存可发现 Runtime 对象的运行时索引。 |
| Runtime Pool | 按某种 Metadata 或能力组织的 Runtime 对象集合。 |
| Runtime Select | 依据 Descriptor、Signature、Runtime Metadata 进行运行时选择。 |
| Static Reflection | 只在编译期存在、不会自动带来 Runtime 成本的反射。 |
| Runtime Reflection | Runtime 中保留的对象摘要与元数据查询能力。 |
| Dynamic Reflection | 支持完整声明观察与修改的运行时反射。 |
| Inspect | 读取 Dynamic Declaration 的完整信息。 |
| Replace | 在兼容性验证后替换运行时实现或绑定。 |
| Runtime Weaving | 在 Runtime 修改 Fragment、Slot 或调用关系。 |
| Slot | 可被扩展或织入的位置。 |
| Fragment | 可应用到 Slot 或声明的实现片段。 |
| Apply | 将 Fragment 应用于 Slot 或声明。 |
| Runtime Select Lifecycle | 初始化、插件装载/卸载或显式请求触发的选择时机。 |
