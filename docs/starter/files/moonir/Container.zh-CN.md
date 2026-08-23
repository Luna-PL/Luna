# src/moonir/Container.cpp

实现 Container.h 声明的 Writer/Reader：在字节层面设计并固化 Moon Container 的文件布局，负责魔数、版本头、目录表、8 字节对齐与 SHA-256 摘要校验。

## 这个文件做什么

把"若干 section 内存对象"翻译为一份自描述、防篡改的二进制，或反向还原。它做了三层防御：

1. **写入侧边界校验**（section 数、字节上限、重复/未知 id、必须 8 个 required 段齐全）——不合格直接失败。
2. **读取侧完整性校验**（魔数、格式版本、头部预留位、目录对齐、区间不重叠、尾随字节、padding 必须为 0、SHA-256 摘要匹配）。
3. **泄密：绝不静默修复**——每一条非法结构都返回 false 并附英文 error 文案。

正是这份文件实现了"先验证再发布"中容器层的"假阳性"防护：一个被截断、被篡改或结构非规范的容器绝不会被当成合法输入。

- 面向 C++ 读者类比：一个手写的、最小化 ELF/archive 解析器，外加摘要做自校验，强调“规范对齐 + 防注入”。

## 结构体·类·常量

| 成员 | 含义 |
| --- | --- |
| constexpr Magic[8] | 魔数 0x89 4d 4f 4f 4e 0d 0a 1a，用于快速识别。 |
| HeaderSize = 80 | 头部固定 80 字节。 |
| DirectoryEntrySize = 32 | 每个目录项固定 32 字节。 |
| DigestOffset = 48 / DigestSize = 32 | SHA-256 摘要在文件中的偏移与长度。 |
| struct DirectoryEntry | 目录表一项：id、flags、offset、length、decodedLength。 |

## 关键函数·方法

| 函数 | 作用 |
| --- | --- |
| digestFor | 用 llvm::SHA256 对除摘要区之外的字节计算摘要（摘要区先置零参与）。 |
| addWouldOverflow / alignEight | 64 位加法溢出检测与 8 字节对齐辅助。 |
| isKnownRequiredSection / hasAllRequiredSections | 判断 id 是否合法、8 个 required 段是否齐全。 |
| zeroRange / fail | 校验区间全为零；统一把 error 置为消息并返回 false。 |
| writeU32/writeU64/readU32/readU64 | 小端 4/8 字节读写小工具。 |
| ContainerWriter::encode | 校验后排序、写目录与各段 payload、写摘要。 |
| ContainerWriter::writeFile | encode 后写二进制文件。 |
| ContainerReader::parse | 逐域读 header 与目录，做完整/对齐/摘要检查后还原 sections。 |
| ContainerReader::readFile | 用 filesystem 取大小、读文件再 parse。 |
| ContainerReader::find | lower_bound 按 id 二分查找。 |

## 与周边文件·阶段的关系

- Container.h：本文件的接口声明；本文件是唯一实现。
- ContainerModel.cpp：调用这里把打包后的字节当最终交付物编码/解码 8 个模型 section。
- 阶段：在 Lowering/Sealer/Verifier 之后，把已验证 Module 落盘或载回。

## 延伸阅读

- src/moonir/Container.h：容器层数据结构。
- src/moonir/ContainerModel.cpp：本层确定的 8 个 section 内“真正装了什么”。
- src/moonir/Verifier.h：模型级完整性校验，与此处的字节级校验互补。


---

---
title: Moon 容器二进制格式的基础类型与读写接口
file: src/moonir/Container.h
namespace: moon
阶段: MoonIR 序列化 / Container 载荷层
---

# src/moonir/Container.h

定义 Moon Container（把一组按 ID 编号的 section 打包成一个自检、带 SHA-256 摘要的二进制容器）的 section 数据类型与 ContainerWriter/ContainerReader 读写接口。

## 这个文件做什么

提供容器级（container 级）的数据结构与接口：把若干 ContainerSection（一个 id + 一段 payload 字节）按既定的文件布局编码/解码成一份可落盘或可传输的二进制。Container 负责**外层信封**：魔数、版本、目录表、8 字节对齐、SHA-256 摘要、大小/重复/越界等完整性与注入校验；它不关心每个 section 内部的内容语义——那属于 ContainerModel.h/.cpp。

- 面向 C++ 读者类比：相当于 ELF section 表的极简版加摘要校验——它只管段如何排布、如何校验完整性，不管段里装什么。

## 关键结构体·类·枚举

| 名字 | 含义 |
| --- | --- |
| enum ContainerSectionId | 8 个 required 段的编号：Manifest=1, Type=2, Symbol=3, Contract=4, Code=5, Imports=6, Exports=7, Sysmeta=8。 |
| OptionalSectionBit = 0x80000000u | 可选段的 id 高位标记；无此位且非已知 required 段会被拒绝。 |
| struct ContainerSection | 一段 payload：id + std::vector<uint8_t> payload。 |
| struct ContainerLimits | 解析/生成的边界限额：最大字节数、最大 section 数、最大字符串/表行/嵌套深度。 |
| class ContainerWriter | 静态：把 vector<ContainerSection> 编码为容器并（可选）写文件。 |
| class ContainerReader | 解析容器字节为 section 列表，支持按 id 查找与格式版本读取。 |

## 关键函数·方法

| 声明 | 作用 |
| --- | --- |
| static bool encode(sections, output, error, limits) | 排序、去重、校验后写成容器字节（含摘要），任何越界/非法输入返回 false 并给出 error。 |
| static bool writeFile(path, sections, error, limits) | 先 encode 再以二进制写入文件。 |
| bool parse(input, error, limits) | 校验魔数/版本/目录/对齐/摘要并还原 section 列表。 |
| bool readFile(path, error, limits) | 读取文件到字节后走 parse。 |
| const ContainerSection* find(uint32_t id) | 二分查找某 id 的 section。 |
| formatMajor()/formatMinor() | 返回容器携带的格式版本。 |

## 与周边文件·阶段的关系

- Container.cpp：上述接口的具体实现与全部字节级逻辑。
- ContainerModel.h/.cpp：上层调用 ContainerWriter::encode / ContainerReader::parse，把整份 Module 的模型拆分、合并到 8 个 section。
- 阶段：位于代码生成（LLVM lowering）之前、前端 Lowering 之后，负责把已验证 MoonIR Module 持久化为容器或从它载入。

## 延伸阅读

- ContainerModel.h：这一层的语义解码，定义每个 section 字节的含义。
- MoonIR.h：Module、DeclarationRecord、TypeRecord 等被 Container 打包的数据结构。
- Verifier.h/.cpp：encode/decode 前后对 Module 的完整性校验，是先验证再发布的执行者。


---

---
title: Moon 容器模型分节编解码的实现
file: src/moonir/ContainerModel.cpp
namespace: moon
阶段: MoonIR 序列化 / Container 语义层
---
