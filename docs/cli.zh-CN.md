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
Windows executable 带 `.exe` 后缀。Native library 必须是 `kind = "library"`，
且 public surface 只能包含普通 Luna export。它生成 `lib<name>.so`（macOS
为 `.dylib`，Windows 为 `<name>.dll`）、同目录文本 IR 和嵌入式 proof section。
构建还会写出 `<library>.trust` 作为安装候选记录，但不会自动搜索或信任；
只有可信 installer 把该精确记录放入显式 trust store 后，未来 Runtime
loader 才可接受。每个 library 还导出 versioned typed descriptor registry，
row 由 proof 的 export digest 绑定并使用 SymbolId/ContractId 身份。内部
verified-loader primitive 使用不可变/私有 staging，只发布与 proof 匹配的
registry entry。EV004 通过 C++17 宿主 API 提供进程内 generation 控制，而不是
面向用户的 load/activation CLI；raw dynamic-loader symbol lookup 不是 trusted 装载路径。

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
头文件仍以 package 末段命名并写在共享库旁。同一 package 不能静默改类为
trusted Native：Native public surface 会拒绝 `export "C" fn`。

开发构建默认使用自身的 `libruntime.a` 和 `clang++`。安装、打包或交叉环境应该
传入 `--runtime-lib` 和 `--cc`，也可设置 `LUNA_RUNTIME_LIB` 与 `LUNA_CXX`。
缺少 runtime 会报告 `DRV0001`，native linker 失败会报告 `DRV0002`。

Native 与 CFFI 链接会在产物旁保留 `<artifact>.luna-link-state`。重复构建时，只有
新生成 IR 和相邻 native `.o` 均逐字节相同、完整链接命令未变，并且编译器、Runtime
archive、object 与路径型链接库都不比产物新，Luna 才输出 `Up to date:`。最终平台链接
只接收 object；若识别到 x86-64 MinGW/Clang executable 工具链，Luna 会直接
调用其配套 `ld.lld`，省去一个冗余 driver 进程。shared library、需要搜索的裸 `-l` 输入、
driver 环境覆盖及其他工具链布局仍经过所选 clang driver；直接链接使用的工具链文件也会
进入失效检查。在直接路径中，lld 把 executable 流式交给 Luna；Luna 写入同目录 pending
文件时同步计算摘要，并只在进程成功结束后发布。native application 还会使用性能指南所述
的前端前保护指纹。
删除 state 文件或任一必要输出会强制执行对应的常规路径。
首次生成的相邻产物与 state 通过同目录 rename 发布；已存在且变化的输出继续使用覆盖写入
与内容校验路径。

### REPL

```sh
luna repl
```

Alpha REPL 只承诺以下经过测试的有限范围：

```text
= 20 + 22
:decl fn twice(value: i32) -> i32 { return value + value; }
:type twice(1)
= twice(21)
print(7)
:undo
:reset
:quit
```

- `= <表达式>` 求值一个结果类型必须为 `i32` 的表达式；
- `:decl <声明>` 校验并持久保存一条完整的单行声明；校验覆盖 LLVM codegen 与
  合成的 REPL 入口，失败的声明不会提交；后续输入会连同已提交声明重新编译；
- `:type <表达式>` 在不执行表达式的前提下报告推导类型；
- `:paste decl`、`:paste expr` 或 `:paste stmt` 读取显式多行 cell，直到单独一行 `:end`；
- `:load <路径>` 将源码文件作为一个声明 cell 校验，并在诊断中保留真实路径和源码摘录；
- 其他输入作为临时 `main` 中的一条语句执行；
- `:undo` 撤销最后一个已提交声明；`:help` 显示本契约，`:reset` 清除声明，
  `:quit` 退出；`exit` 作为 `:quit` 的兼容写法保留。

它的状态是 **Implemented Experimental**，不是持久运行时：局部变量、堆值、JIT
全局状态和运行时状态都不会跨输入保留。声明校验、类型查询、表达式和语句均在全新
worker 中编译；可执行 cell 也在其中完成 JIT 和运行。会话等待输入时，会在隔离门
后恰好保留一个尚未使用的一次性预热 worker。cell 到达后才以有界、长度定界的
“操作/虚拟路径/源码”请求提交；该 worker 只消费这一个请求。每个 worker 都会初始化
LLVM 的不可变 target registry 后才发布 ready；不同 cell 之间不共享编译器、JIT 或
用户状态。执行结束后先显式刷新 cell 输出并发布完成信号。请求以及 worker 的有界
running/final 两阶段结果记录都通过
继承的数据通道传输，父进程在执行期间持续排空结果；ready、隔离门和完成通知使用原生
Windows Event 或继承的 POSIX socketpair，不再轮询控制文件。捕获的 stdout/stderr
也改为共享字节预算、分别持续排空的通道，因此每个 cell 不再创建临时文件，也不再轮询
文件大小；内部协议端点会在链接代码运行前取消后代继承。后台回收器最多容纳两个已完成 worker，每个从结果发布起
最多获得 500 ms 正常退出时间，超时便终止整个受控进程树；替代 worker 的异步准备
也从结果发布后开始。因此交互思考时间可以隐藏进程与 LLVM 动态库启动成本，同时编译器、JIT、链接库和
运行时状态仍不能跨 cell。连续脚本输入可能早于下一 worker 就绪，仍会承担剩余启动
等待。空闲 worker 会监视父进程；会话消失时自行退出。链接库的进程退出 hook 仅属
尽力清理，不属于 cell 输出语义，也必须在同一 500 ms 回收宽限内完成。

因此编译器崩溃、运行时 abort 或超时不会终止 REPL 会话。`--timeout <秒>` 设置每个 cell 包括编译和执行
在内的 1–3600 秒限制（默认 30 秒）；`--memory-limit <MiB>` 将 worker 进程树限制
在 256–65536 MiB（默认 1024），`--output-limit <MiB>` 将 stdout 与 stderr 合计
限制在 1–1024 MiB（默认 16）。正常完成的后代最迟在 500 ms 回收宽限结束时清理；
取消路径立即清理。
这是崩溃和资源隔离，不是操作系统安全沙箱；只应运行可信源码和链接库。使用
AddressSanitizer 的开发构建必须预留巨大的影子地址区，因此会禁用内存上限；
超时、输出和进程树隔离仍然有效。多行输入必须显式进入 paste 模式，不依赖
parser 错误猜测。脚本输入可使用 `--no-prompt`。`--timings` 为每次 `validate`、
`type` 或 `run` 操作向 stderr 写入一行 `timing[repl]`：
`cache=hit` 表示命中会话内“源码完全一致且此前成功”的 `:type` 结果缓存；该缓存最多
32 项、合计 1 MiB，声明变化、`:undo` 和 `:reset` 都会使其失效。验证与执行结果绝不
缓存，因此可执行 cell 仍使用全新 worker 并重复其副作用。缓存命中时会报告
`prewarmed=unused`，且所有 worker 阶段字段均为零。
`prewarmed=yes` 表示所选 worker 在 cell 提交前已经到达隔离门，`prewarmed=no`
表示 `total` 包含剩余启动等待；提交前的空闲预热时间不计入 `total`。
`frontend` 是源码分析与 MoonIR 的聚合耗时，细分字段为 `lexer`、`parser`、
`semantic`、`traits`、`ownership`、`indexing`、`lowering`、`verification`、
`sealing` 和 `moon-opt`；由于计时取整，细分之和不要求与聚合值完全相等。
`codegen` 表示 LLVM 生成，`codegen-setup` 表示生成器构造与 worker 启动阶段尚未完成的
target 初始化，`codegen-total` 是两者的完整包络。`jit-materialize` 与 `jit-lookup`
表示 ORC JIT 准备，
`execution` 仅表示用户入口函数调用，`jit-cleanup` 表示 LLJIT 清理，`jit-total`
是完整 `jitRun()` 包络。`overhead`
表示父进程观测到但不在 `worker-total` 内的时间，`total` 是父进程观测到的完整提交
延迟。`worker-request` 表示请求接收与解码，`worker-link` 表示动态库装载，
`worker-running-publish` 表示执行前的 Running 状态帧发布，
`worker-total` 从请求通道处理开始持续到最终结果序列化之前，`worker-other` 是其中
未归入请求、链接、frontend、codegen 包络、Running 发布或 JIT 包络的部分。细分阶段为零可能表示低于
计时精度或尚未执行。提交之外继续进行的空闲准备、worker 清理和替代进程创建不计入
`total`。`-O0/-O2/-O3`、`--opt`、`--link`、`--timeout`、
`--memory-limit`、`--output-limit`、`--timings` 和 `--help` 均作为正式 REPL CLI
选项解析。
父进程延迟时间线另外报告：`parent-cache` 是完整缓存命中路径；`parent-acquire`
取得已准备的 worker，`parent-submit` 完成就绪/隔离并发送请求，`parent-roundtrip`
从提交持续到观察到完成、因此与 worker 阶段重叠，`parent-collect` 排空并验证结果，
`parent-replenish` 安排回收和替代 worker。成功操作的这些父进程字段会分割 `total`，
不得再与 worker 阶段相加。`parent-roundtrip-gap` 是 `parent-roundtrip` 减去
`worker-total`，它混合最终结果序列化/发布、信号唤醒、调度和跨进程测量噪声，并非
单一代码阶段。
Windows 的内存限制是 Job Object 的进程树总量限制；Linux worker 会在发布 ready
之前设置不可自行提高、由后代继承的逐进程 `RLIMIT_AS`。Darwin 不允许把上限降到
启动时已映射的地址空间以下，因此 macOS 会把所选额度加到初始映射占用后，再设置
同样不可自行提高的上限。

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
| `--moon-cost-report` | `run`, `build` | 输出运行时、泛型实例和 kernel 的显式成本。 |
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
