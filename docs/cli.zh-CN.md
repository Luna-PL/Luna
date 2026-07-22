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
```

`check` 会执行词法、解析、语义、trait、所有权检查，以及 MoonIR 降级、优化和
验证，但不会进入 LLVM 代码生成。因此，没有 `main` 的库 package 应优先使用
这个命令。

### JIT 运行

```sh
luna run <文件或package> [-O0|-O2|-O3] [选项]
```

`run` 将验证后的 MoonIR 降级到 LLVM，使用 JIT 编译并执行 `main`。驱动进程的
退出状态就是 Luna 程序的退出状态。

### AOT 构建

```sh
luna build <文件或package> [-O0|-O2|-O3] [选项]
```

文件输入 `build app.luna` 会生成 `app.luna.ll` 和 `app`；package 目录会在目录
中生成 `<package-id>.ll` 和 `<package-id>`。Windows 可执行文件带 `.exe` 后缀。
更多说明见 [AOT 构建](aot_build.md)。

### REPL

```sh
luna repl
```

当前 REPL 会把每行输入包装到临时 `main` 中并通过 JIT 运行。它仍是实验性工具，
尚不是能够持久保留声明的完整交互环境。

## 常用选项

| 选项 | 适用命令 | 含义 |
|---|---|---|
| `-O0`, `-O2`, `-O3` | `run`, `build` | 选择 MoonIR/LLVM 优化级别，默认 `-O0`。 |
| `--opt O2` | `run`, `build` | 优化级别长写法，也支持 `--opt=O2`。 |
| `--link <库>` | `run`, `build` | 为 JIT 加载共享库，或增加 AOT 链接依赖；可重复。 |
| `--emit-moonir <路径>` | `check`, `run`, `build` | 输出经过验证和优化的文本 MoonIR。 |
| `--moon-cost-report` | `run`, `build` | 输出运行时、动态绑定、泛型实例和 kernel 的显式成本。 |
| `--gpu-target <列表>` | `run`, `build` | 为逗号分隔的目标生成设备代码。 |
| `--reserve-kernel-runtime` | `run`, `build` | 即使没有可达 launch，也保留 kernel runtime 能力。 |
| `--runtime-lib <路径>` | `build` | 指定 AOT 链接使用的 Luna `libruntime.a`。 |
| `--cc <编译器>` | `build` | 指定 C++ 链接驱动。 |

接受参数的长选项同时支持 `--名称=值`。

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

插件限制及环境选择规则见[外部 fragment 插件](dynamic_plugins.md)。

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
luna build app.luna -O2 \
  --runtime-lib /opt/luna/lib/libruntime.a \
  --cc /usr/bin/clang++ \
  --link m
```
