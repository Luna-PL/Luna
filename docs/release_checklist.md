# Luna 0.2.0-alpha 发布检查清单

## 已完成

- [x] 版本元数据统一为 `0.2.0-alpha`。
- [x] `luna --version` 可直接报告版本。
- [x] Linux CI 配置存在，覆盖稳定核心构建与 CTest。
- [x] macOS 原生 CI 已通过，跨平台 CMake 路径不依赖 Linux/发行版硬编码。
- [x] Windows 原生 CI 已在 MSYS2 UCRT64 上通过，包含 `.exe` AOT、ORC JIT 和原生动态库加载路径。
- [x] 非硬件 CTest 回归通过，包含 JIT/AOT、包、FFI、所有权、CPS、优化和外部片段 ABI。
- [x] 默认关闭的基础 CPU benchmark 已加入，并明确区分 JIT、AOT 构建、AOT 执行和 C++23 执行指标。
- [x] 安装结果包含驱动、运行时库、Runtime/Plugin ABI 头文件和文档。
- [x] 已知限制、GPU 前置条件和实验性语义已记录。
- [x] 构建目录和编译生成物已加入忽略规则。
- [x] 采用 MIT / Apache-2.0 双许可证，并随安装结果发布许可证文本。

## 发布前由发布机确认

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build -LE hardware --output-on-failure
sudo cmake --install build --prefix /opt/luna
/opt/luna/bin/luna --version
```

在全新环境中，还应使用安装后的 `luna`、`libruntime.a` 和一份独立的
`.luna` 文件完成一次 JIT 与 AOT 运行。AOT 必须显式传递
`--runtime-lib /opt/luna/lib/libruntime.a` 和 `--cc`，或设置对应环境变量。

## 可选硬件门

- ROCm：确认 `rocminfo` 可见目标 GPU，再启用
  `-DLUNA_ENABLE_ROCM_SMOKE=ON -DLUNA_ROCM_SMOKE_ARCH=gfx*`。测试会显式
  编译该 GPU 目标，并在执行时设置 `LUNA_GPU_BACKEND=rocm`。
- CUDA：需要 NVIDIA 驱动和 CUDA Driver API；当前仓库没有 NVIDIA 硬件 CI。

硬件测试未通过时不能标记为编译器核心回归；应记录为发布环境缺少设备、驱动
或运行时。Alpha 不把硬件测试设为无 GPU 主机上的默认门。

## 不应在 Alpha 发布前偷偷纳入的内容

- 跨包依赖、锁文件和远程仓库解析。
- 外部 `context`/`resume()`、多发射或带词法捕获的共享库插件。
- 通用堆拥有容器、完整 GPU 容器模型和稳定的源语言 profiling API。
