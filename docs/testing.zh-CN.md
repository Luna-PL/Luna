# 测试与回归

Luna 的稳定核心使用 CTest 执行语义回归。该套件同时覆盖应通过的程序和应拒绝的程序；它不把源语言 `main` 的非零返回值当作编译失败，而是检查编译器打印的 `Program exited with code:` 行或结构化诊断。

核心错误还带有稳定错误码；具体格式和当前公开编号见
[错误模型的诊断编号](reference/error_model.md#10-编译器诊断编号)。

## 运行

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

发布候选构建应同时把项目自身的警告提升为错误：

```sh
cmake -S . -B build -DLUNA_STRICT_WARNINGS=ON
cmake --build build --parallel
```

Linux CI 在 C++17/C++23 构建矩阵中都启用该门禁。它只作用于 Luna
仓库拥有的 target，不把 LLVM 或系统头文件的第三方警告误算为项目回归。

内存安全与未定义行为门禁可独立启用：

```sh
cmake -S . -B build-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DLUNA_ENABLE_SANITIZERS=ON \
  -DLUNA_STRICT_WARNINGS=ON
cmake --build build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build-sanitized -LE hardware --output-on-failure
```

该选项用 ASan/UBSan 插桩编译器及进程内 JIT runtime，但不污染安装用的
`libruntime.a`，因此安装后的 AOT 测试不需要额外链接 sanitizer runtime。
ORC 生成的入口函数没有 Clang UBSan 的函数类型前缀元数据，故仅 JIT 入口的
一次间接调用关闭 `function` 检查；调用前后的编译器与 runtime 仍保持插桩。

当前 `luna.semantic-regressions` 覆盖：

- lexer/parser 的结构化错误与顶层多错误恢复、基础函数、算术/位/关系运算符和逻辑短路、闭包、泛型单态化、`const` / `constexpr`、编译期反射。
- interceptor / context / slot 的有效路径、abort 合流与多发射资源安全负例。
- 函数、类型和 trait 的版本选择与不完整 impl。
- 多文件包、跨文件解析恢复、显式导出、重复符号/版本身份、导出/FFI 边界与包名一致性。
- C ABI 的 JIT/AOT `puts` 链接、线性 `free` 移交，以及 ABI、泛型和参数类型边界的负例。
- 线性资源的路径敏感所有权：`if` 的双路径消费/单路径泄漏、提前 `return` 的终止路径、循环零次/多次执行边界，以及不可达语句。
- GPU in-flight buffer、未 `await` event 与 event 在提前返回前的释放要求。
- CPU 模拟器下的基础、版本化和 move-event 异构计算。
- `Drop`、`rc`/`arc` 的显式克隆和最终释放，以及 Result 两个 variant 的活动载荷清理。
- `Result` 构造/判别/解包、`?` 的成功与错误提前返回、错误路径 cleanup、fragment 边界拒绝和 abort 型 panic。

ROCm 与 CUDA 的实机测试不包含在默认 CTest 中：默认测试必须在无 GPU 的 CI 主机上运行。ROCm 冒烟测试在具备 AMD GPU 的主机上额外执行：

```sh
LUNA_GPU_BACKEND=rocm ./build/luna run examples/heterogeneous.luna \
  --gpu-target=rocm:gfx1101
```

也可在配置阶段启用 `-DLUNA_ENABLE_ROCM_SMOKE=ON` 并通过
`-DLUNA_ROCM_SMOKE_ARCH=gfx1101` 指定目标 ISA，然后执行
`ctest --test-dir build -L rocm --output-on-failure`。该可选测试会分别验证 ROCm 的 JIT 与 AOT kernel 路径。

CPU 对照基准可通过 `-DLUNA_ENABLE_CPU_BENCHMARK=ON` 启用，详情见
[性能基准](benchmarks.zh-CN.md)。

设置 `-DLUNA_ENABLE_ROCM_BENCHMARK=ON` 会额外注册 `luna.rocm-cpp23-comparison`：它用 16,777,216 个元素、十轮变换对照 Luna AOT 与 C++23/HIP，并校验两者结果。它带 `benchmark` label，不在默认测试集内。

每次修复语义或诊断问题时，应同时添加一个最小正例或负例，并在 `tests/semantic_regressions.cmake` 中断言稳定的输出或关键诊断文本。

`luna.package-export-abi` 是独立的 AOT ABI 测试：它确认 package 中带有 `export` 的函数在 LLVM IR 中为外部符号，而未导出的函数为 `internal`。测试在结束时删除自身生成的 `.ll` 与可执行文件。

`luna.return-cleanup-abi` 是路径敏感释放的 AOT 测试：它确认嵌套分支和落空路径上的每个 `return` 都在自身路径上发射一次携带精确 `size/alignment` 的 `rt_dealloc`，而不是把清理留在不可达的块末尾。

`luna.result-error-aot` 比较 Result 传播案例的 JIT/AOT 输出，并检查 AOT IR
同时保留 `try.error`、资源清理和 unwrap panic 边界。

`luna.control-flow-aot` 确认两支均 `return` 的条件语句可生成、链接并运行有效的 AOT LLVM IR；语义回归同时拒绝非 `unit` 函数中未覆盖的返回路径。

`luna.ffi-aot` 通过系统链接器构建并运行 C FFI 示例，补足 JIT 进程符号解析之外的 ABI 路径。

`luna.jit-aot-parity` 对同一含标准输出和非零返回码的程序比较 JIT 与 AOT 的退出码及 stdout，覆盖算术/位运算、关系比较和短路控制流。

`luna.optimization-pipeline` 检查 `-O0/-O2/-O3` 入口，并对比 `-O0` 与 `-O2`
的 IR：局部栈槽应被提升、常量计算应折叠；它还验证直线 reduction loop 的有界
O3 四路展开提示，并防止同一提示应用于过小的嵌套递归，同时优化后的 JIT/AOT
必须返回相同结果。

`luna.fragment-lowering-abi` 检查静态 fragment 不会退化为动态候选选择或堆分配；`luna.structured-cps-abi` 则在 O0 下检查 context 续体的栈上 frame、独立入口和返回分发块，并运行“续体内 return”案例，确认 `resume()` 后的代码不会错误执行。

`luna.external-fragment-plugin-abi` 使用真实共享库验证外部描述符的 ABI、注册、重复契约拒绝和显式参数调用；`luna.external-fragment-dispatch` 则让动态槽选择静态候选之外的外部 interceptor，确认插件继续动作后槽续体仍然执行。

`luna.aot-runtime-boundary` 覆盖显式 `--runtime-lib` / `--cc`、缺失运行时库的 `DRV0001` 诊断，以及 AOT 可执行文件的 GPU 后端初始化失败边界。

`luna.install-smoke` 把当前构建安装到隔离的临时前缀，确认驱动、静态
runtime、公开 ABI 头文件、标准库 workspace 与语义参考文档均已安装；随后只用
安装树完成 `--version`、`check`、JIT 和显式 runtime/compiler 的 AOT
构建与运行。该测试带 `release` 和 `install` label，是发布包布局的自动化门禁。

`luna.compiler-identity` 会比较源码仓库当前 commit 与编译器 structured analysis hello
报告的 commit。CMake 同时监视 Git HEAD 与当前 branch ref，因此普通增量构建会在提交
后刷新该身份，不会静默发布带旧 build stamp 的二进制。

`luna.runtime-gpu-error-state` 验证 CPU 模拟器下 event 的成功与无效状态 ABI；`luna.gpu-error-boundary-abi` 检查 AOT IR 中 `await` 的失败分支会调用统一 GPU 错误终止入口，保证 CUDA/ROCm 的 launch 或同步失败不会静默继续执行。
前者同时检查 Runtime error snapshot 的稳定 GPU domain/code、两阶段消息复制和
旧 `last_error` 兼容，并确保一次 operation error 不会反向污染已成功的 backend
初始化状态；`luna.runtime-abi-v1` 覆盖 fragment plugin 错误快照以及公开头文件的
C/C++ ABI 可编译性。

`luna.jit-aot-extended-parity` 以 `-O2` 比较多文件包和 CPU 模拟器异构程序的 JIT/AOT 退出码及 stdout，确保优化不破坏包级链接或 host-side launch/event 降低。

`luna.stable-core-parity` 是第二周的完整一致性矩阵：它对稳定核心示例在 `-O0`、`-O2` 与 `-O3` 下分别比较 JIT/AOT 的 stdout、退出码和 stderr，覆盖泛型、反射、闭包、ADT、版本、trait、FFI、多文件包与 CPU 模拟器异构程序。它还防止 ADT 形参被误判为被调用方拥有的堆内存，从而在正常调用后发生重复释放。
