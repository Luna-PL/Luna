# Luna 编译器命令参考

[English](cli.md) | [简体中文](cli.zh-CN.md)

`luna` 驱动可以接收一个独立 `.luna` 文件或一个 package 目录。不带参数运行
`luna` 会输出内建用法；当前 Alpha 尚未提供单独的 `--help` 别名。

## 命令

### 版本

```sh
luna --version
luna -V
luna version
```

三种形式都会输出编译器版本。

### 检查

```sh
luna check <文件或package> [--emit-moonir <路径>]
           [--message-format=json]
```

`check` 会执行词法、解析、语义、trait、所有权检查，以及 MoonIR 降级、优化和
验证，但不会进入 LLVM 代码生成。因此，没有 `main` 的库 package 应优先使用
这个命令。

`--message-format=json` 会切换到供编辑器和 CI 使用的 `luna.diagnostic` JSONL
version 1。stdout 严格包含一条 `hello`、零条或多条 `diagnostic` 和一条
`summary`，stderr 保持为空。磁盘文件位置使用绝对路径、UTF-8 byte offset 和
exclusive end。退出码 `0` 表示无错误，`1` 表示已报告诊断，`2` 表示命令或协议
用法错误。

### 分析

```sh
luna analyze <文件或package> --message-format=json
luna analyze <文件或package> --message-format=json \
    --overlay <文档> < current-buffer.luna
luna analyze <文件或package> --message-format=json \
    --overlays-from-stdin < overlays.json
```

`analyze` 输出编译器拥有的 `luna.analysis` version 1 语义快照：一条 `hello`、声明、
已解析的直接函数、trait method、用户类型语法、trait、struct 字段访问、
enum variant 构造与 match 引用记录，以及一条
`summary`。使用 `--overlay` 时，stdin 会在内存中替换所选根
package 内一个已存在的源码文件，不写临时文件。`--overlays-from-stdin` 读取
`luna.overlay` version 1 JSON 对象：

```json
{
  "protocol": "luna.overlay",
  "version": 1,
  "overlays": [
    {"path": "/workspace/src/api.luna", "text": "..."},
    {"path": "/workspace/src/main.luna", "text": "..."}
  ]
}
```

列出的文件会原子替换所选根 package 中已存在的源码，不写临时文件；重复路径、外部路径或
依赖 package 路径会被拒绝。其他文件和依赖仍走正常解析。客户端必须在选择传输前检查
`single-document-overlay` 或 `multi-document-overlay` capability。

### JIT 运行

```sh
luna run <文件或package> [-O0|-O2|-O3] [选项]
```

`run` 将验证后的 MoonIR 降级到 LLVM，使用 JIT 编译并执行 `main`。驱动进程的
退出状态就是 Luna 程序的退出状态。

### 产物构建

```sh
luna build <package> [-O0|-O2|-O3] [选项]
```

正式产物构建必须输入包含 `luna.package` 的目录，manifest 必须显式选择
`kind = "application"` 或 `kind = "library"`。standalone 文件仍可用于 `check`、
`run` 和 `analyze`，但 `build` 会拒绝。`-t native` 是默认目标；Native
application 生成 `<package-root>/build/native/<package末段>` 及同目录文本 LLVM IR，
Windows executable 带 `.exe` 后缀。Native library 在 proof section 与 loader 完成前失败关闭。

`luna build <package目录> -t moon` 要求 manifest 显式声明 `kind`，并生成经过
自验证的 host-specific `.moon`。`-o` 可覆盖路径；否则输出为
`<package-root>/build/moon/<package末段>.moon`。application 必须恰有一个
package `main`，library 不得有 `main`；standalone source 和 native linker/GPU artifact
选项会被拒绝。编译器输入可保留 generic recipe，但容器只写入 concrete
instance；作为 export 或 entrypoint 的 generic recipe 会被拒绝。

`luna build <package目录> -t cffi` 只接受 manifest 中声明为 library 的 package，
并要求至少一个 `export "C" fn`。普通 `export fn`、application、package `main`、零 C
导出和 standalone source 都会被拒绝。默认输出为
`<package-root>/build/cffi/lib<package末段>.so`（macOS 为 `.dylib`，Windows 为
`<package末段>.dll`）以及同目录的 `<package末段>.h`；`-o` 覆盖共享库完整路径，
头文件仍以 package 末段命名并写在共享库旁。当前 Native library 会在证明段发射完成前
明确拒绝，不能降级为无证明共享库或 executable。

开发构建默认使用自身的 `libruntime.a` 和 `clang++`。安装、打包或交叉环境应该
传入 `--runtime-lib` 和 `--cc`，也可设置 `LUNA_RUNTIME_LIB` 与 `LUNA_CXX`。
缺少 runtime 会报告 `DRV0001`，native linker 失败会报告 `DRV0002`。

### REPL

```sh
luna repl
```

Alpha REPL 只承诺以下经过测试的有限范围：

```text
= 20 + 22
:decl fn twice(value: i32) -> i32 { return value + value; }
= twice(21)
print(7)
:reset
:quit
```

- `= <表达式>` 求值一个结果类型必须为 `i32` 的表达式；
- `:decl <声明>` 校验并持久保存一条完整的单行声明；后续输入会连同这些声明重新编译；
- 其他输入作为临时 `main` 中的一条语句执行；
- `:help` 显示本契约，`:reset` 清除声明，`:quit` 退出；`exit` 作为
  `:quit` 的兼容写法保留。

它的状态是 **Implemented Experimental**，不是持久运行时：不支持多行输入；局部
变量、堆值、JIT 全局状态和运行时状态都不会跨输入保留。无法写成单行的声明应放入
源码文件并使用 `luna run`。

## 常用选项

| 选项 | 适用命令 | 含义 |
|---|---|---|
| `-O0`, `-O2`, `-O3` | `run`, `build` | 选择 MoonIR/LLVM 优化级别，默认 `-O0`。 |
| `--opt O2` | `run`, `build` | 优化级别长写法，也支持 `--opt=O2`。 |
| `--link <库>` | `run`, `build` | 为 JIT 加载共享库，或增加 AOT 链接依赖；可重复。 |
| `-t native|moon|cffi` | `build` | 选择 native（默认）、host-specific Moon Container 或 C ABI shared library + header。 |
| `-o <路径>` | `build` | 覆盖 native、Moon 或 CFFI 主产物输出路径。 |
| `--emit-moonir <路径>` | `check`, `run`, `build` | 输出经过验证和优化的文本 MoonIR。 |
| `--message-format=json` | `check`、`analyze` | 输出对应命令的 versioned JSONL 协议。 |
| `--overlay <文档>` | `analyze` | 从 stdin 读取一个内存源码替换。 |
| `--overlays-from-stdin` | `analyze` | 从 stdin 读取带版本的多文档 overlay JSON 对象。 |
| `--moon-cost-report` | `run`, `build` | 输出运行时、动态绑定、泛型实例和 kernel 的显式成本。 |
| `--gpu-target <列表>` | `run`, `build` | 为逗号分隔的目标生成设备代码。 |
| `--reserve-kernel-runtime` | `run`, `build` | 即使没有可达 launch，也保留 kernel runtime 能力。 |
| `--runtime-lib <路径>` | `build` | 指定 AOT 链接使用的 Luna `libruntime.a`。 |
| `--cc <编译器>` | `build` | 指定 C++ 链接驱动。 |

接受参数的长选项同时支持 `--名称=值`。

`-O0` 保留最直接的实验性控制流 IR，是 Alpha 默认值；`-O2` 运行标准 LLVM
速度优化管线，`-O3` 使用更积极的管线，并为小型、直线、无调用的 while loop
提供有界的四路展开提示。宿主 module 在优化前后都会验证，AOT
也把同一级别传给 native compiler。设备 kernel 使用独立的目标相关 O3 管线。

## GPU target 与运行时后端

设备产物生成和运行时后端选择是两个独立决策：

```sh
LUNA_GPU_BACKEND=rocm luna run app.luna -O2 \
  --gpu-target=rocm:gfx1101
```

`--gpu-target` 接受 `sim`、`cuda[:sm_*]` 和 `rocm[:gfx*]`，多个目标以逗号分隔。
不请求硬件目标，就不会支付对应 code object 的生成成本。

运行时由 `LUNA_GPU_BACKEND` 选择 `sim`、`cuda` 或 `rocm`，默认是 `sim`。显式
选择不可用后端会明确失败，不会静默回退。具体见[异构计算](heterogeneous_compute.md)。

## 环境变量

| 变量 | 用途 |
|---|---|
| `LUNA_RUNTIME_LIB` | 未传 `--runtime-lib` 时的默认 AOT Runtime ABI 库。 |
| `LUNA_CXX` | 未传 `--cc` 时的默认 AOT 链接驱动。 |
| `LUNA_GPU_BACKEND` | 运行时 GPU 后端：`sim`、`cuda` 或 `rocm`。 |
| `LUNA_GPU_PROFILE=1` | 输出 CUDA/ROCm device-event 累计 kernel 时间。 |
| `LUNA_GPU_DUMP_HSACO=<目录>` | 保存生成的 ROCm HSACO 以供检查。 |
| `LUNA_FRAGMENT_PLUGIN=<路径>` | 加载 Alpha v1 外部 fragment 插件。 |
| `LUNA_FRAGMENT_<SLOT>=<名称>` | 为 slot 选择已链接的动态 fragment 候选。 |

插件限制及环境选择规则见[Fragment 与插件](fragments.md)。

## 常用工作流

检查库 package 并查看 MoonIR：

```sh
luna check path/to/library --emit-moonir library.moonir
```

使用可移植模拟器运行并查看显式成本：

```sh
LUNA_GPU_BACKEND=sim luna run examples/full_showcase/app -O2 \
  --moon-cost-report
```

使用安装后的编译器进行可复现 AOT 构建：

```sh
luna build path/to/application-package -O2 \
  --runtime-lib /opt/luna/lib/libruntime.a \
  --cc /usr/bin/clang++ \
  --link m
```
