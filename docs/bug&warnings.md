# bug & warnings

## Windows JIT/AOT 与异构计算修复状态

2026-07-22 修复已在 Linux、macOS 与 Windows GitHub runner 通过；真实
ROCm JIT/AOT 也已在外部设备验证：

1. JIT 使用 ORC `absoluteSymbols` 显式注册实际被 LLVM 模块引用的全部
   Luna `rt_*` 函数，不再依赖可执行文件导出表、`-rdynamic` 或
   `__declspec(dllexport)`。
2. kernel runtime 入口由 MoonIR `features.kernel` 控制；纯 CPU 和未使用
   kernel 不生成 `rt_gpu_*` 引用，JIT/AOT 的含 kernel 模块使用相同入口检查。
3. PTX/HSACO 使用显式 `--gpu-target` 选择；`LUNA_GPU_BACKEND` 仅用于生成
   程序的运行时选择。离线或交叉 AOT 无需编译机存在可用 GPU，设备初始化
   推迟到生成程序执行。
4. AOT 链接已从 `std::system()` 字符串命令改为 LLVM
   `ExecuteAndWait` 参数数组，消除 CMD/MSYS2 二次解析引号的问题。
5. `windows-ci.yml` 已恢复，并单独运行 JIT/AOT parity、显式 runtime symbol、
   GPU target/runtime split、AOT runtime boundary 和 MoonIR cost boundary 后再
   运行完整非硬件测试。

本机 Linux 的 simulator、JIT/AOT parity 和离线 AMDGPU ISA 回归已通过；
真实 ROCm 的 JIT 与 AOT 均输出 `42` 并正常退出。

## 待确定的机制

1. external fragment plugin v2 如何接收 `LunaRuntimeModuleContextV1`，并在不暴露
   栈 continuation 的前提下支持 `context/resume()`。
2. 将 C FFI 的 `linear raw<T>` allocator/deallocator 约定升级为可验证的
   foreign release capability，并定义 C struct/union 布局导入。
3. Moon 容器验证、加载授权与 hotspot/JIT 的 W^X 可执行内存策略。
4. CUDA 硬件 CI 和更多 ROCm 架构的长时间验证。
5. 语言原生并发仍延后，不纳入当前 Runtime ABI/ownership 收敛工作。
