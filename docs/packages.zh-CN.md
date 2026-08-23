# Package、module 与显式导出

Luna 将发布单元与源码命名空间分开：

- **Package** 是版本、依赖、签名、缓存和 Moon 容器的单元。
- **module** 是 package 内的源码命名空间。一个 package 可以有多个 module，
  module 可以有子 module。
- **workspace** 组织多个相关的本地 package；package 本身没有语义子包。

## 命名规则

Package ID 使用由 `.` 分隔的反向 DNS 名称：

```luna
package org.luna.std;
package com.example.graphics.vulkan;
```

module 使用 `::` 表示层级：

```luna
module io;
module io::format;
module collections::ordered;
```

因此，`.` 只属于 Package ID，`::` 只属于语言名称路径。

`com.example.graphics` 和 `com.example.graphics.vulkan` 是两个完全独立的 package。
后者不会自动继承前者的依赖、可见性或版本。反向 DNS 前缀表示发布命名权，
不表示语言父子关系。

## 源文件头

每个源文件最多声明一个 package 和一个 module，顺序为 package、module、using：

```luna
package com.example.application;
module commands::build;

using org.luna.std as std;
using com.example.serialization as serde;
```

`using` 引用的是 package，不是 module。`as` 别名必须在当前 package 内唯一；
同一别名不能指向两个 Package ID，package 也不能 `using` 自己。未声明
`module` 的文件属于 package root module，用于兼容现有源码。

完全限定路径为：

```luna
std::io::print("value = {}", value);
serde::json::decode<Data>(source);
```

其中 `std` / `serde` 是 package alias，`io` / `json` 是 module，末尾是声明。

## 本地 package 组装

对于没有 manifest 的目录输入，驱动会将目录下按文件名排序的 `.luna` 文件组装
成兼容 package。对于带 `luna.package` 的输入，则按照 `sources` 递归装载源码。
所有显式 `package` 声明必须一致，但每个文件可以声明不同 module：

```text
application/
  01_io.luna       # module io;
  02_format.luna   # module io::format;
  03_main.luna     # module application;
```

PackageManager 已保留 module 图和 `Package ID -> alias` 依赖边，MoonIR 也保留
`moon.source_module` 和 `moon.using`。带 manifest 的 package 使用下面的严格最小 TOML
schema：

```toml
# luna.package
[package]
id = "com.example.application"
version = "1.0.0"
kind = "application"
sources = ["src"]

[dependencies]
"org.luna.std" = "0.2.1"
```

`kind` 是必填字段，只能是 `"application"` 或 `"library"`。application
必须在 package root 中定义且仅定义一个 `main`，library 不得定义 `main`。
`sources` 必须是 package 目录内的相对文件或目录，不允许绝对路径或 `..`
逃逸。目录中的 `.luna` 文件递归枚举并按路径排序。源码里每个 `using`
都必须有对应 `[dependencies]` 项。

```toml
# luna.workspace
[workspace]
members = ["core", "std", "application"]
```

PackageManager 从当前 package 向上查找最近的 `luna.workspace`，再读取各 member
的 `luna.package`，用规范 Package ID 定位本地依赖。它不会隐式搜索其他
父目录、网络或系统库路径。

```toml
# luna.lock（工具生成并按 Package ID 规范排序）
[[package]]
id = "org.luna.std"
version = "0.2.1"
source = "workspace:std"
hash = "..."
```

当前 Alpha 本地 resolver 要求精确版本，并核对 lock 中的 Package ID、version 和
workspace source。`hash` 字段已是必填的非空完整性槽位，但在 Moon 容器/注册表
产物格式冻结前尚不计算或验证内容摘要。

resolver 会递归装载本地依赖闭包。声明身份由 Package ID、module path 与源码名称
共同构成：未限定名称只在当前 module 查找；同一 package 的其他 module 使用
`module::symbol`；依赖声明使用 `alias::module::symbol`。不同 module 可以安全声明
同名符号，编译器会生成彼此隔离且确定性的链接名。跨 Package ID 的引用必须指向
`export` 声明，而 package 内跨 module 引用不要求 `export`。

```luna
// org.luna.fixture.app / module application
using org.luna.fixture.core as core;

fn main() -> i32 {
    return core::values::library_value();
}
```

依赖 package 自己的 `using` 别名保持在其所有者命名空间内，因此不同 package 可以
重复使用同一个别名而不互相污染。该所有权关系与每个依赖声明的 Package ID/module
身份均会保留到 MoonIR，并由 verifier 检查。

对于没有 `main` 的库 package，使用 `luna check <package>` 完成 lexer 到验证后
MoonIR 的全路径检查，不生成 LLVM IR 或可执行文件。

## 显式导出

只有写出 `export` 的声明进入 package 公共接口；未导出函数、类型和片段仍属于
package 内部。`export` 是 ABI 承诺，不是单纯的名称解析标记。未导出包级
函数在 LLVM IR 中保持内部链接；导出函数使用外部符号。

函数、结构体、枚举、trait、`interceptor` 与 `context` 均可导出。`extern`
函数不能同时导出。Metadata/Selector 的公开接口规则见
[versioning.md](versioning.md)。
