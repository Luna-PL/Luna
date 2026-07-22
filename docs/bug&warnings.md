# bug & warnings

## Windows JIT/AOT 与异构计算修复状态

2026-07-22 已完成候选修复，等待 GitHub Windows runner 和真实 ROCm
设备的外部验证：

1. JIT 使用 ORC `absoluteSymbols` 显式注册实际被 LLVM 模块引用的全部
   Luna `rt_*` 函数，不再依赖可执行文件导出表、`-rdynamic` 或
   `__declspec(dllexport)`。
2. kernel runtime 入口由 MoonIR `features.kernel` 控制；纯 CPU 和未使用
   kernel 不生成 `rt_gpu_*` 引用，JIT/AOT 的含 kernel 模块使用相同入口检查。
3. PTX/HSACO 的编译期选择不再初始化 CUDA/HIP；离线或交叉 AOT 无需编译机
   存在可用 GPU，设备初始化推迟到生成程序执行。
4. AOT 链接已从 `std::system()` 字符串命令改为 LLVM
   `ExecuteAndWait` 参数数组，消除 CMD/MSYS2 二次解析引号的问题。
5. `windows-ci.yml` 已恢复，并单独运行 JIT/AOT parity、显式 runtime symbol、
   AOT runtime boundary 和 MoonIR cost boundary 后再运行完整非硬件测试。

本机 Linux 的 simulator、JIT/AOT parity 和离线 AMDGPU ISA 回归已通过。
Windows CI 和真实 ROCm JIT/AOT 只有在外部执行完成后才能标记为已验证。

## 待确定的机制

1. 既然我们使用结构化类型，那么函数自然应该允许接受结构化类型，而不一定是名义类型，但是对于结构化类型，label versioning机制也许需要变动，考虑类似using语法强化名义类型的类型安全
2. 标签需要更精确的机制，比如可以用一些运算符确定精确版本和最大兼容版本等，可能需要引入require语句
3. 确定versioning机制的发生时间。我倾向于按需支付性能成本地支持动态选取，其他时候则确定静态化选择，可能需要引入dynmaic修饰
4. 语言原生级别并发的设计问题
5. 异构计算的基座设计问题
