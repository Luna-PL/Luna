# Luna 0.2.1 预发布说明

> 文档类别：发布说明
> 适用版本：Luna 0.2.1
> 状态：Implemented Experimental
> 规范性：部分规范
> 实现核对：待 0.2.1 发布提交确认（2026-08-01）

[English version](alpha_release.md)

## 定位

Luna 0.2.1 是用于语言试验和内部项目的 Linux/macOS/Windows 预发布编译器原型。三个平台的稳定核心均由 CI 工作流验证；Windows 使用 MSYS2 UCRT64 工具链。稳定核心包括多文件包与显式导出、验证后的 MoonIR、基础函数/泛型/ADT、线性所有权与借用、Runtime ABI v1、显式 C FFI、JIT/AOT、`-O0/-O2/-O3`，以及 CPU 模拟器上的初始异构 ABI。

默认回归覆盖语义负例、包、导出 ABI、FFI、控制流清理、JIT/AOT 一致性、优化、运行时边界和 CPU GPU 模拟器。硬件 ROCm 冒烟测试为可选项，见 [heterogeneous_compute.md](heterogeneous_compute.md)。

## 长期维护状态

`0.2.1` 是当前长期维护版本线，不是通往近期 Beta 的短暂过渡标签。目前没有
预定的 Beta、`0.3` 或语言版本升级日期。公开版本字符串继续保持
`0.2.1`，不同构建和工具链快照以源码 commit、发布 manifest 和校验和区分，
不能仅凭相同版本字符串认定二进制完全相同。

带 tag 的正式 prerelease 产物保持不可变；发布工作流不会覆盖已经存在的
`v0.2.1` Release。后续同版本开发快照必须以 commit 标识，不能静默替换
已发布归档。

近期开发资源转向工具链：

- 诊断质量、源码定位和错误解释；
- formatter、language server 与编辑器集成；
- build、test、package、workspace 和本地依赖工作流；
- 安装、预编译分发、可复现构建和跨平台可靠性；
- benchmark、profiling、调试和开发者检查工具。

语言核心不再以扩大语法或类型表面为近期目标。仍允许修复正确性/安全缺陷、实现与
冻结契约不一致、缺失负例以及阻塞工具链的内部接口问题。任何新的语言能力必须作为
独立决策重新评估版本影响，不得借工具链工作隐式进入稳定核心。

## 已知限制

- 显式 `interceptor` / `context` / `slot`、`constexpr` / 反射、复杂 trait 版本组合均仍是实验性语义。
- 外部片段插件已提供 Alpha v1 ABI，但仅支持 host-only、single-shot、显式参数的 `interceptor`；`context`、`resume()`、多发射和词法捕获仍不可跨共享库边界。
- Runtime ABI v1 已固定 allocator、console、外来资源和可选可执行内存的宿主边界；Moon 容器加载、验证和 hotspot/JIT 策略仍属于后续 MoonRuntime。
- 已有安全定长 `array<T, N>`、只读借用 `slice<T>`、数组字面量和带边界检查的索引；可变切片与堆拥有容器仍未开放。GPU 数据模型当前仍以 `device_buffer<i32>`、一维 grid 与固定 256 线程 block 为主；host 批量复制使用低层 `raw<i32>` 指针。
- 大规模 ROCm 对照可与 C++23/HIP 执行同一 64 MiB、十轮变换工作负载。设置 `LUNA_GPU_PROFILE=1` 可输出 CUDA/ROCm device-event 累计 kernel 时间；它目前是 runtime profiling 开关，尚未形成稳定的源语言计时 API。
- CUDA 代码生成已实现，但尚未在本仓库的 NVIDIA 硬件 CI 上验证。
- ROCm 冒烟测试需要可见 AMD GPU、HIP 运行时、内核驱动和匹配的 `--gpu-target=rocm:gfx*`。基础 JIT/AOT 冒烟已在 RX 7800 XT 上通过；更广泛的 GPU 型号、长时间稳定性与性能数据仍待积累。
- Package 已支持严格的 `luna.package`、本地 `luna.workspace`、`luna.lock`、
  精确版本依赖和递归 workspace 依赖装载；远程 registry、内容摘要验证、缓存和
  网络依赖解析尚未实现。
- AOT 安装后须显式传入 `--runtime-lib` / `--cc`，或设置 `LUNA_RUNTIME_LIB` / `LUNA_CXX`。
- REPL 仅支持 `= <i32 表达式>`、`:decl <完整单行声明>`、单行临时语句和
  `:help/:reset/:quit`。声明通过源码重编译保留；局部变量、堆值、JIT 全局状态和
  运行时状态不会跨输入保留，也不支持多行输入。

## 预编译包

Git tag `v0.2.1` 触发发布工作流；工作流只有在对应平台完整非硬件 CTest
通过后才上传以下带 SHA-256 校验文件的归档：

| 归档 | 支持范围 |
|---|---|
| `luna-0.2.1-linux-x86_64.tar.gz` | Ubuntu 24.04、x86_64、glibc |
| `luna-0.2.1-macos-<arch>.tar.gz` | macOS 14；`<arch>` 是发布 runner 的实际架构 |
| `luna-0.2.1-windows-ucrt64-x86_64.zip` | Windows x86_64、MSYS2 UCRT64 |

归档包含 `luna`、静态 Luna Runtime、公开 Runtime ABI 头、标准库、文档和编译器
运行所需的 LLVM 动态库（Windows 包含 UCRT64 动态依赖闭包）。每个归档根目录的
`RELEASE-MANIFEST.txt` 记录目标、依赖和源码提交。

这是预编译编译器分发，不是完全自包含的 native SDK：

- `luna check`、CPU JIT 与模拟 GPU 后端可直接使用包内编译器；
- `luna build` 仍需要目标平台兼容的 LLVM 22 `clang++`，并应显式传入
  `--runtime-lib <解压目录>/lib/libruntime.a` 与 `--cc <clang++>`；
- CUDA/ROCm 仍要求宿主驱动、用户态 runtime 和匹配硬件；
- 不承诺在表中目标之外的发行版、旧版 macOS、MSVC ABI 或 MSYS2 MSVCRT 环境
  运行。

## 升级承诺

Alpha 稳定核心的错误码和安全语义将尽量保持兼容；实验性能力仍可能调整。退出
`0.2.1` 长期版本线必须经过单独的版本决策、语义基线复核、迁移说明和完整
发布门，不由时间经过或工具链功能完成自动触发。每次语义变化都必须添加正例、负例
与迁移说明。

## 发布检查清单

- [x] 版本元数据与 `luna --version` 统一为 `0.2.1`。
- [x] Linux C++17/C++23、macOS 和 Windows UCRT64 原生 CI 覆盖稳定核心。
- [x] Linux 自有 target 使用严格警告，并执行完整 ASan/UBSan 非硬件回归。
- [x] 非硬件 CTest 覆盖语义、包、FFI、所有权、CPS、优化和外部插件 ABI。
- [x] 安装树包含驱动、runtime、公开 ABI 头文件、标准库 workspace 和文档。
- [x] `luna.install-smoke` 从隔离安装树验证 `--version`、`check`、JIT 和 AOT。
- [x] 构建产物已忽略，历史 `build-cpp23` 工作树产物已删除。
- [x] MIT / Apache-2.0 双许可证随安装结果发布。
- [x] Release workflow 构建、测试、打包三个目标并生成 SHA-256 文件。
- [ ] `v0.2.1` tag 上三个打包 job 与 publish job 实际通过。
- [ ] GitHub prerelease 的三份归档及校验文件完成下载复验。

发布机最终确认：

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLUNA_STRICT_WARNINGS=ON
cmake --build build --parallel
ctest --test-dir build -LE hardware --output-on-failure
sudo cmake --install build --prefix /opt/luna
/opt/luna/bin/luna --version
```

正式目标前缀还应重复安装后 JIT/AOT。AOT 必须显式传递匹配的
`--runtime-lib` 与 `--cc`，或设置对应环境变量。

ROCm 发布门要求可见设备、驱动和匹配 ISA；CUDA 仍缺少本仓库 NVIDIA 硬件 CI。
无 GPU 主机不把硬件失败算作编译器核心回归。

Alpha 封版不纳入远程 registry/网络依赖、外部 context/resume、多发射插件、通用
堆拥有容器或稳定源语言 profiling API。
