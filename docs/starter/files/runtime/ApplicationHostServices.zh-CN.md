# src/runtime/ApplicationHostServices.cpp

Luna 应用级宿主服务的完整实现，在 `ApplicationHostServices.h` 声明的两个工厂函数之后，实现了控制台输入（stdin 读取）和跨平台文件系统（POSIX + Windows 原生 API）。

## 这个文件做什么

- 实现 `lunaApplicationConsoleV1()` —— 返回一个包含 stdin 控制台输入能力的 `LunaConsoleV1` 常量。
- 实现 `lunaApplicationFileSystemV1()` —— 返回一个完整的 11 函数文件系统 `LunaFileSystemV1` 常量。
- 管理文件描述符到 `LunaFileHandleV1`（`uint64_t`）的映射（`FileRegistry` 类，线程安全）。
- 通过 `nativePath`、`nativeOpen`、`nativeRead` 等平台适配函数，统一封装 POSIX（`open`/ `read`/ `write`/ `lseek`/ `fsync`/ `close`/ `fstat`/ `lstat`/ `unlink`/ `mkdir`）和 Windows（`_wopen`/ `_read`/ `_write`/ `_lseeki64`/ `_commit`/ `_close`/ `_fstat64`/ `_wstat64`/ `_wunlink`/ `_wmkdir`）。
- 将平台原生 `errno` 错误码映射到 Luna `LunaIoErrorKindV1` 枚举值。
- 验证 UTF-8 路径合法性（`validUtf8` 函数，手动扫描 UTF-8 序列并检查过短编码、surrogate 对、超界码点等）。

## 关键结构体·类·枚举

### `FileRegistry`（匿名命名空间类）

| 成员 | 类型 | 用途 |
|---|---|---|
| `mMutex` | `std::mutex` | 保证 `insert`、`withDescriptor`、`take` 的线程安全 |
| `mDescriptors` | `std::unordered_map<LunaFileHandleV1, int>` | Luna 句柄 → 原生文件描述符的映射 |
| `mNextHandle` | `LunaFileHandleV1` | 单调递增的句柄生成器，从 1 开始 |

关键方法：
- `insert(int descriptor)` —— 插入原生 fd，返回新的 `LunaFileHandleV1`，跳过 `LUNA_INVALID_FILE_HANDLE_V1`（0）。
- `withDescriptor(handle, function)` —— 线程安全地按句柄查询 fd 并执行回调，返回回调结果；句柄不存在返回 `EBADF`。
- `take(handle, descriptor)` —— 从映射中移除句柄并返回原生 fd，用于关闭操作。

### 平台适配常量

| 常量 | 值 | 用途 |
|---|---|---|
| `applicationConsole` | `LunaConsoleV1` | 支持 `consoleWrite` + `consoleFlush` + `consoleRead`（stdin） |
| `applicationFileSystem` | `LunaFileSystemV1` | 完整 11 函数文件系统 |

## 关键函数·方法

### 工厂函数

| 函数 | 用途 |
|---|---|
| `lunaApplicationConsoleV1()` | 返回 `&applicationConsole`，包含 stdin 控制台输入 |
| `lunaApplicationFileSystemV1()` | 返回 `&applicationFileSystem`，包含完整文件系统 |

### 控制台回调

| 函数 | 用途 |
|---|---|
| `consoleWrite` | 向 `stdout`/ `stderr` 写入 `fwrite` |
| `consoleFlush` | 刷新 `stdout`/ `stderr` 缓冲区 |
| `consoleRead` | 从 `stdin` 读取 `fread`，失败时通过 `std::clearerr` 清除错误标志并映射 `errno` |

### 文件系统回调（11 个函数）

| 函数 | 底层平台调用 | 用途 |
|---|---|---|
| `openFile` | POSIX: `open` / Windows: `_wopen` | 解析标志位（READ/WRITE/APPEND/TRUNCATE/CREATE/CREATE_NEW），打开文件并注册句柄 |
| `readFile` | `read` / `_read` | 读取文件内容 |
| `writeFile` | `write` / `_write` | 写入文件内容 |
| `seekFile` | `lseek` / `_lseeki64` | 定位文件指针 |
| `flushFile` | 仅验证句柄有效 | 对普通文件是空操作 |
| `syncFile` | `fsync` / `_commit` | 同步到磁盘 |
| `closeFile` | `close` / `_close` | 从注册表移除句柄并关闭文件 |
| `handleMetadata` | `fstat` / `_fstat64` | 按文件句柄查询元数据 |
| `pathMetadata` | `lstat` / `_wstat64` | 按路径查询元数据（POSIX 用 `lstat` 以读取符号链接自身） |
| `removeFile` | `unlink` / `_wunlink` | 删除文件 |
| `createDirectory` | `mkdir` / `_wmkdir` | 创建目录（POSIX 模式 0777） |

### 辅助函数

| 函数 | 用途 |
|---|---|
| `validUtf8` | 手动验证 UTF-8 序列：检查单字节、双字节（0xc2-0xdf）、三字节（0xe0-0xef）、四字节（0xf0-0xf4）序列的合法性，拒绝过短编码、surrogate 对（U+D800-U+DFFF）、超界码点（>U+10FFFF） |
| `validPath` | 组合 UTF-8 验证和空字节检查（路径中不允许嵌入 NUL） |
| `nativePath` | POSIX: 直接 `assign`；Windows: 通过 `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` 转换为 `std::wstring` |
| `errorKind` | 将 `errno` 值映射到 `LunaIoErrorKindV1` 枚举（如 `ENOENT`→`NOT_FOUND`，`EACCES`→`PERMISSION_DENIED`） |
| `ioFailure` | 构造 `LunaIoErrorV1` 并返回 `LUNA_RUNTIME_STATUS_IO_ERROR` |
| `metadataType` | 将平台 `st_mode` 转换为 `LunaFileTypeV1`（REGULAR/DIRECTORY/SYMLINK/OTHER） |

## 与周边文件·阶段的关系

- **ApplicationHostServices.h** —— 本文件实现的工厂函数声明，包含 `RuntimeABI.h`。
- **RuntimeABI.h** —— 提供 `LunaConsoleV1`、`LunaFileSystemV1`、`LunaFileHandleV1`、`LunaIoErrorV1`、`LunaFileMetadataV1` 等类型定义，以及所有 `LUNA_*` 常量。
- **Runtime.cpp** —— 将本文件提供的 `lunaApplicationConsoleV1` 和 `lunaApplicationFileSystemV1` 组合到 `applicationHostServices` 常量中，通过 `rt_install_application_host_services_v1` 安装。
- 本文件是 Luna 运行时中唯一直接调用平台 POSIX/Windows 文件 I/O API 的翻译单元。

## 延伸阅读

- `ApplicationHostServices.h` 中两个工厂函数的声明
- `RuntimeABI.h` 中 `LunaFileSystemV1` 和 `LunaConsoleV1` 的完整字段定义
- `Runtime.cpp` 中 `applicationHostServices` 常量的构造与激活
- POSIX `open`(2)、`lseek`(2)、`fsync`(2)、`fstat`(2) 等系统调用文档
- Windows CRT `_wopen`、`_lseeki64`、`_commit`、`_fstat64` API 文档


---

---
title: ApplicationHostServices.h
source: src/runtime/ApplicationHostServices.h
language: zh-CN
audience: Luna 运行时实现者 / 应用嵌入者
---

# src/runtime/ApplicationHostServices.h

Luna 应用级宿主服务的工厂函数声明头文件，提供了两个返回进程级生命周期服务表指针的函数：`lunaApplicationConsoleV1` 和 `lunaApplicationFileSystemV1`。

## 这个文件做什么

- 声明 `lunaApplicationConsoleV1` —— 返回一个包含控制台输入（stdin 读取）的 `LunaConsoleV1*` 服务表。
- 声明 `lunaApplicationFileSystemV1` —— 返回一个完整的 `LunaFileSystemV1*` 文件系统服务表。
- 区分于 `Runtime.h` 中的默认服务：默认 Runtime 故意不暴露输入和文件系统；应用级服务通过显式安装启用。
- 仅包含一个 `#include "RuntimeABI.h"` 依赖。

## 关键结构体·类·枚举

本文件不定义任何结构体或枚举，仅引用 `RuntimeABI.h` 中定义的：

- `LunaConsoleV1` —— 控制台 I/O 服务表（含 `write`、`flush`、`read` 三个函数指针）。
- `LunaFileSystemV1` —— 文件系统服务表（含 11 个函数指针）。

## 关键函数·方法

| 函数 | 返回类型 | 用途 |
|---|---|---|
| `lunaApplicationConsoleV1()` | `const LunaConsoleV1*` | 返回进程级控制台服务表，支持 stdout/stderr 输出和 stdin 输入 |
| `lunaApplicationFileSystemV1()` | `const LunaFileSystemV1*` | 返回进程级文件系统服务表，支持打开、读写、定位、同步、关闭、元数据、删除文件、创建目录等全部操作 |

这两个函数实现位于 `ApplicationHostServices.cpp`，返回的是文件内部静态常量对象的指针，因此生命周期与进程一致。

## 与周边文件·阶段的关系

- **RuntimeABI.h** —— 提供 `LunaConsoleV1` 和 `LunaFileSystemV1` 的类型定义。本文件直接依赖它。
- **ApplicationHostServices.cpp** —— 本文件声明的两个函数的实现，包含完整的平台适配代码（POSIX + Windows）。
- **Runtime.h / Runtime.cpp** —— `Runtime.cpp` 中的 `applicationHostServices` 常量通过调用这两个函数来初始化其控制台和文件系统子表。`rt_install_application_host_services_v1` 安装这个服务。
- **生成代码（Generated IR）** —— 编译器生成的应用程序入口点通过 `rt_install_application_host_services_v1` 安装这些服务，然后使用 `rt_console_write_v1`、`rt_file_open_v1` 等转发函数。

## 延伸阅读

- `ApplicationHostServices.cpp` 中各函数的具体平台实现
- `Runtime.cpp` 中 `applicationHostServices` 常量的构造与使用
- `RuntimeABI.h` 中 `LunaConsoleV1` 和 `LunaFileSystemV1` 的完整字段定义
