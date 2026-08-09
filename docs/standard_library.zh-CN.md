# Luna 标准库设计

> 文档类别：面向 0.3 的设计与实现边界
> 当前实现基线：Luna 0.2.1
> 状态：Stage A 与 Stage B host/allocator substrate 已实现；安全容器与 I/O
> adapter 仍是 Proposed

在 package、resource、Moon container 和 Runtime ABI 契约稳定前，标准库继续位于主仓库。
0.2.1 源码只作为历史迁移基线保留；新的拥有型容器 API 面向 clean-break 0.3 编译器。
本文中的 API 草图定义语义，不冻结最终 0.3 拼写。

## 1. 目标与非目标

第一个可用标准库必须提供：

- console 输入输出和显式 byte/text I/O；
- 文件、路径和常用整文件 helper；
- 安全的可增长堆容器 `Vec<T>`；
- 使用同一分配模型的 UTF-8 拥有文本 `String`；
- `Option<T>`、`Result<T, E>`、具体错误与静态转换；
- 支撑上述 API 的最小比较、转换、迭代和 resource trait。

首版不提供 exception、通用动态装箱 `Error`、async I/O、网络、线程、locale-aware
formatting、自定义 allocator、非 UTF-8 路径或庞大 collection 目录。这些能力需要独立的
lifetime、ABI 和可移植性工作。

## 2. Package 与依赖边界

目标 package graph 必须无环：

```text
org.luna.core                         # 无 package 依赖；仅共享单元 Runtime ABI
       ^       ^
       |       |
org.luna.sys  |                       # 原始 Runtime/OS 边界，仅标准库内部
       ^       |
       |       |
org.luna.alloc                         # Core + Sys allocator capability
       ^
       |
org.luna.std                           # Core + Alloc + Sys host service
```

用依赖表示：

- `core`：无 package 依赖；`Rc`/`Arc` 只调用固定的 Luna 共享单元 Runtime ABI；
- `sys -> core`；
- `alloc -> core + sys`；
- `std -> core + alloc + sys`。

`sys` 不是便利的用户 API，也不由 prelude 重导出。它保留原始 status code、handle、size 和
ABI version；`alloc` 与 `std` 才是安全 adapter。安全 wrapper 绝不能通过不同的 allocator
或 host domain 释放 resource。

workspace 已包含以下 dependency skeleton：

```text
stdlib/
├── core/   # org.luna.core
├── sys/    # org.luna.sys
├── alloc/  # org.luna.alloc
└── std/    # org.luna.std
```

## 3. Module 清单

| Package | 首批 module | 职责 |
|---|---|---|
| Core | `prelude`、`option`、`result`、`error`、`convert`、`cmp`、`iter`、`resource`、`Rc`、`Arc` | 值/语义协议与使用固定 Luna Runtime 分配域的共享容器 |
| Sys | `alloc`、`console`、`fs`，后续 `env`/`clock` | 版本化 Runtime ABI import 与平台 status |
| Alloc | `boxed`、`vec`、`string` | Luna 拥有堆值与分配失败 |
| Std | `io`、`fs`、`path`、`fmt`、`env`、`process`、`time` | 安全 host-facing API |

初始 prelude 保持小型：`Option`、`Result`、`From`/`TryFrom`、`Drop`、`Clone`、iterator
trait 以及可选的 `rc(value)`/`arc(value)` 普通 helper。
不默认导入文件系统、console、process 或网络操作。未来 `std::prelude` 可重导出 `Vec`
和 `String`，但它们的规范身份仍属于 `org.luna.alloc`。

## 4. 错误与转换模型

可恢复失败仍是普通 `Result<T, E>`，进程失败仍是 `panic`。标准库不引入 exception
unwinding 或全局错误 registry。

0.3 方向：

- `core::option::Option<T>` 保持规范可选容器身份；
- `core::result::Result<T, E>` 成为规范声明型 Result 身份；
- 编译器只对该 Core Result 的精确身份实现 `?`，而不是识别任意名为 `Result` 的 enum；
- `From<S> for T` 保持一跳、静态、唯一且可审计；
- `TryFrom<S>` 和 parse trait 返回具体错误类型；
- 库 API 返回最窄的有用错误，应用自行定义聚合 enum。

Core 保留不分配的 `BoundsError`、`UtfError`、`LayoutError` 和 `AllocError`等值错误；
Std 增加 host 错误。初始 I/O 错误必须在不分配诊断文本时保留机器事实：

```text
IoError
├── kind: IoErrorKind
├── raw_code: Option<i64>
└── operation: IoOperation
```

`IoErrorKind` 首批包含 `NotFound`、`PermissionDenied`、`AlreadyExists`、`InvalidInput`、
`UnexpectedEof`、`Interrupted`、`WouldBlock`、`Unsupported` 和 `Other`。
`Read::read` 的 EOF 是 `Ok(0)`，不是错误。平台可读文本延迟格式化，不属于错误身份。

不分配错误很重要：分配失败本身不得依赖构造 `String` 或 `Vec`。

## 5. Allocation 与 `Vec<T>`

`Vec<T>` 是标准库 nominal type，不是 compiler builtin。其概念状态为：

```text
Vec<T>
├── data: T 的 Luna-owned allocation
├── length: 已初始化元素数
├── capacity: 已分配元素数
└── allocator domain: 首版固定为 Global Luna allocator
```

首版只支持 Runtime 激活前安装的进程级 Luna allocator，不暴露用户自定义 allocator，
也不允许接管 foreign memory。即使紧凑 runtime 表示中不存每值 allocator pointer，allocator
身份仍是 resource contract 的一部分。

必须保证：

- `length <= capacity`；
- 恰好 `[0, length)` 已初始化；
- 增长会检查 `size_of<T> * capacity` 溢出和 alignment；
- reserve 失败不改变原 vector；
- move-out 先清除元素初始化状态，cleanup 不得再观察已移走元素；
- Drop 对每个剩余已初始化元素恰好析构一次，再通过原 Luna allocator domain 释放；
- zero-sized type 和零 capacity 不依赖可解引 null pointer；
- shared/mutable slice 借用 vector，借用期间禁止使借用失效的增长。

最小语义 API：

```text
Vec::new() -> Vec<T>
Vec::try_with_capacity(usize) -> Result<Vec<T>, AllocError>
len / capacity / is_empty
as_slice / as_mut_slice
get / get_mut -> Option<borrow>
try_push(T) -> Result<unit, AllocError>
push(T)                         # convenience；分配失败时 panic
pop() -> Option<T>
try_reserve(usize) -> Result<unit, AllocError>
truncate / clear
```

`insert`、`remove`、`retain`、`append`、`split_off`、自定义 allocator 和 spare-capacity API
属于后续工作。索引可以在越界时 panic；`get`/`get_mut` 是可恢复替代。

现有 `FromIterator` builder 不能报告分配失败。0.3 保留 OOM 时 panic 的便利型不可失败
`collect`，并增加可失败 builder/terminal（`TryFromIterator`/`try_collect`）以支持恢复。
不得把可失败 `push` 隐藏在声称不可失败的 protocol 中。

### 5.1 安全实现前置条件

通用 `Vec<T>` 只在以下契约存在后开始实现：

1. generic recursive Drop glue 与 exact-once cleanup；
2. heap element 的稳定 move-out/initialization tracking；
3. mutable slice 借用与增长失效规则；
4. 库代码可用的 layout overflow/alignment query；
5. 不解引 null 便可报告失败的 Runtime allocation 操作；
6. resource contract 中的 release-domain 事实；
7. 可失败 iterator collection 语义。

前置 1 与 4–6 现已具备：泛型名义实例已有编译器派生的递归 Drop 与 exact-once 字段
cleanup；Runtime ABI v1 提供 checked array layout、无需分配的
`LunaAllocErrorV1`、事务式可失败 alloc/realloc 和 Global Luna release-domain 事实；
`org.luna.sys::alloc` 负责转发，不解析 host table。前置 2、3 与 7 刻意留在 0.3
language boundary。Core `Rc<T>`/`Arc<T>` 已作为不依赖 element move-out 的第一个
普通拥有型容器纵向切片完成；`Vec<T>` 仍等待其余前置。

在此之前，定长 `array<T, N>` 和借用 `slice<T>` 仍是安全容器基线。

### 5.2 借用 byte 与 text view

0.3 mutable slice 的最终语法尚未冻结，但行为已经确定。byte view 是带 source provenance
且不拥有资源的 `{data, length}`：

- `slice<T>` 是 shared loan，只允许读取；
- mutable slice 是唯一 exclusive loan，允许原地替换元素，并对旧值执行 exact cleanup；
- 在类型系统能证明已补回元素或暴露 initialization state 前，不允许通过 mutable view
  move-out 元素；
- view 不得比 source 存活更久；zero-length view 仍保留 loan 与 provenance；
- 任意 view 存在时都拒绝会改变 Vec capacity 的操作，包括发生重分配的 reserve；不增长的
  元素修改仍遵守 shared/exclusive loan；
- `slice<u8>` 表示任意 bytes。借用 text view 是带有效 UTF-8 invariant 的 shared bytes；
  mutable raw bytes 未经重新验证不能成为 text。

safe path 边界使用借用 text 合约。Sys 把同一组 bytes 和显式 length 传给 host filesystem
table，不发生到 `cstr` 的隐式转换；需要 NUL-terminated 平台 path 的 adapter 必须先检查内嵌 NUL。

## 6. 文本

`String` 属于 Alloc，并且只拥有有效 UTF-8。为了实用 I/O 和 formatting，还需要借用的
`str`-like view；它的最终 0.3 拼写是语言设计问题。即使拼写改变，语义分层保持不变：

- owned `String`：使用 Luna allocator 的可增长 UTF-8 bytes；
- borrowed text view：带 shared loan 与 UTF-8 invariant 的 `{bytes, length}`；
- `slice<u8>`：任意 bytes，不隐式代表有效文本；
- `cstr`：不拥有的 NUL-terminated FFI 边界，不是通用 string view。

最小操作包括构造、byte/text view、`len_bytes`、`is_empty`、`try_push_char`、
`try_push_str`、UTF-8 验证以及与 `Vec<u8>` 之间带具体 `UtfError` 的转换。Unicode normalization、
grapheme segmentation、locale 和 regex 不是首批要求。

当前 builtin `string` 保留为 0.2 兼容类型。其 0.3 迁移必须是显式 desugaring/兼容步骤，
不得默认认为与 `alloc::String` ABI 等价。

## 7. I/O 与文件

### 7.1 Byte I/O protocol

Std 先实现 byte-oriented 同步协议：

```text
Read::read(&mut self, &mut [u8]) -> Result<usize, IoError>
Write::write(&mut self, &[u8]) -> Result<usize, IoError>
Write::flush(&mut self) -> Result<unit, IoError>
Seek::seek(&mut self, SeekFrom) -> Result<u64, IoError>
```

库 helper 提供 `read_exact`、`read_to_end`、`read_to_string` 和 `write_all`，并正确保留部分操作与
`Interrupted` 语义。text decoding 必须显式。

`Stdin`、`Stdout` 和 `Stderr` 是实现这些协议的普通 host handle。扩展后的 Runtime Console v1
合约支持 stdout/stderr write、flush 和可选 stdin read。默认 runtime 不声明 input capability，
普通 generated application 会显式安装 native application profile；embedding host 可提供并保留
自己的 service table。首批便利 API 可包含
`print`、`println` 和 `read_line`，但它们必须路由到这些 protocol，不得另建第二套 console ABI。

0.2.1 workspace 现在提供一层刻意临时、类型明确的 `std::io`，使应用可在 0.3
trait 与 owned String 语法冻结前使用 host substrate：

```text
stdout_write_text(cstr) / stdout_write_line(cstr) -> i32 status
stderr_write_text(cstr) / stderr_write_line(cstr) -> i32 status
stdout_write_i32(i32) / stdout_write_i32_line(i32) -> i32 status
stderr_write_i32(i32) / stderr_write_i32_line(i32) -> i32 status
stdout_flush() / stderr_flush() -> i32 status
read_line_lossy() -> borrowed cstr
parse_i32_or(cstr, fallback) -> i32
```

同时提供由 caller 传入 buffer 和 error record 的 raw byte 读写 wrapper。
`read_line_lossy` 使用 thread-local 4096-byte buffer，同一线程下次调用会覆盖其内容，
较长输入会被截断，且无法区分空行、EOF 和 host failure。这些限制已体现在命名与
文档中；该兼容层不是未来 `Read`/`Write`/`Display`/`FromStr` 合约。

Formatting 位于 `Write` 之上。`Display`、`Debug` 和 formatter 可逐步加入。第一个可用版本
可仅支持 primitive 和 string；compile-time format-string 校验值得实现，但不是原始 byte/text
output 的前置条件。

### 7.2 文件系统

`std::fs` 首批提供：

```text
File::open(path) -> Result<File, IoError>
File::create(path) -> Result<File, IoError>
OpenOptions::{read, write, append, truncate, create, create_new}
File::{read, write, flush, seek, metadata, sync_all, close}
fs::{read, read_to_string, write, exists, metadata, remove_file, create_dir, create_dir_all}
```

`File` 拥有 opaque host handle。显式 `close` 可报告失败。Drop 只做 best-effort close，因为 Drop 不能
返回 Result；需要 close durability 的调用者必须显式调用 `flush`/`sync_all`/`close`。

Runtime ABI v1 现已暴露可选的版本化 filesystem capability，包含
open/read/write/seek/flush、stat、close 和基本 path 操作。调用使用 status + caller-owned out parameter，
不返回借用的进程全局错误字符串。
handle 不得作为 Luna heap pointer 暴露。

第一版 path API 只接受有效 UTF-8 路径，提供借用 `Path` 和拥有 `PathBuf`。能保留平台原生
非 UTF-8/UTF-16 的 `OsStr`/`OsString`、目录迭代、link、permission 和 canonicalization 属于后续。
这项限制必须明确，不得静默替换非法 path data。

## 8. 其他必要基础

以下能力是必需的，但不必全部进入第一个实现切片：

1. **Core trait：** `Drop`、`Clone`、`Default`、`From`、`TryFrom`、`Eq`、`Ord`、`Hash` 和 iterator
   protocol。编译器合作必须使用精确 Core 身份。
2. **拥有 resource：** 已有普通 Core `Rc<T>`/`Arc<T>`；在 Vec 之外仍需要
   `Box<T>`。Arc 的跨线程入口必须等待 compiler-derived thread-safety sysmeta。
3. **Collection：** `HashMap`/`HashSet` 等 Vec、String、Eq、Hash 和 panic/failure cleanup 稳定后再做。
   deque 是后续专用化，不是前置。
4. **Host utility：** `env`、`process` 和 monotonic `time` 在各自 Runtime capability 版本化后加入。
   process spawning 与 process exit/arguments 分开设计。
5. **Parse 与 format：** 数值 parse、`FromStr`、`Display` 和 `Debug`；formatting 必须增量写出，
   不得强制先分配中间 String。
6. **Math：** 语义可移植的无 host 数学函数可属于 Core；依赖 target/libm 的操作位于 Std/Sys 边界。

网络、async task、线程/同步、随机源、动态库、serialization、regex 和完整 Unicode 属于后续
package 或 milestone。它们不得阻塞第一个可用 IO/Vec/error surface。

## 9. 实现顺序

### Stage A：契约与 package skeleton

- 已实现：集中定义 Result、From、Drop、Clone、iterator 与 resource 的精确
  0.3 Core 身份；编译器已一次性切换，不保留 0.2 language branch；
- 已实现：`org.luna.sys` 与 `org.luna.alloc` dependency skeleton；
- 已实现：append-only console-input 与 filesystem Runtime ABI capability 合约，并兼容已发布的
  v1 结构前缀；
- 已实现：mutable byte/text view 语义；最终 0.3 语法刻意保持开放。

### Stage B：第一个可用纵向切片

native application-host substrate 已实现：generated JIT/AOT entry 会在不覆盖 embedding host 的
前提下安装 stdin/filesystem service，并提供 opaque handle、UTF-8 path 校验、partial I/O、
结构化错误、metadata、seek、sync 与 exact-once close。可恢复 allocator substrate 也已实现，
包含 overflow 检查、无分配错误、zero-size 语义和失败保持 realloc。`org.luna.sys` 已拥有
raw `console`、`fs` 和 `alloc` forwarding wrapper，Luna library code 不需要解析 C
service table。以下 safe Luna API 仍等待 0.3 language surface 的一次性落地。

- 已作为临时 0.2.1 adapter 实现：明确类型的 stdout/stderr text 与 i32 write、flush、
  raw byte I/O、lossy line input 和 fallback-based i32 parse；
- 已实现：普通名义 `Rc<T>`/`Arc<T>`、显式 `Clone`、递归 Drop callback 与
  Runtime ABI v1 retain/release；
- `IoError` 与不分配错误映射；
- 在 Global Luna allocator 上实现 `Box<T>`、`Vec<T>` 和 `String`；
- `try_push`、`try_reserve`、`write_all` 和基本整文件 helper；
- 覆盖成功、分配失败、提前 return、move-only 元素、部分 I/O、close 行为和 UTF-8 错误的
  JIT/AOT 测试。

### Stage C：常规应用 surface

- stdin、File/OpenOptions、Path/PathBuf、metadata 和目录创建；
- Display/Debug 和 checked format string；
- 可失败 iterator collection 到 Vec/String；
- env、process arguments/exit 和 monotonic time。

### Stage D：更广标准库

- HashMap/HashSet 与更完整 text/path support；
- 网络、并发与 async 只在独立设计后加入。

## 10. 验收标准

标准库能力只有在以下条件全部满足时才算完成：

- 只有一个文档化 owner package，不存在反向依赖；
- 明确成功、可恢复失败、panic 和 Drop 行为；
- allocator/handle release domain 精确；
- 包含提前 return 和 move-only payload 的正反所有权测试；
- 适用时具有分配失败与部分 I/O 测试；
- JIT/AOT parity 与 installed-tree coverage；
- 替换 0.2 compiler builtin 时具有迁移说明。

这个边界在 0.3 语法与 resource model 稳定前，有意优先小而一致的标准库，而不是广泛 API 目录。
