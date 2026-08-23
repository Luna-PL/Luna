# src/codegen/CodeGeneratorGpu.cpp —— GPU 内核码物（PTX/HSACO）生成、设备指针解析与 launch 发射

## 这个文件做什么

本文件实现所有 GPU 相关的 LLVM 代码生成：`emitKernelPTX`（把宿主内核函数克隆到 NVPTX 模块，替换索引为 `blockIdx*blockDim+threadIdx`，编译为 PTX 文本）、`emitKernelHSACO`（克隆到 AMDGPU 模块，地址空间转换+私有 alloca 重写，编译为 HSACO 文件并通过 lld 链接为动态 HSA 码对象，最后包装进 HIP Module Bundle 格式）、`generateDeviceBufferPointer`/`generateHostRawPointer`（解析设备/主机内存指针路径）、`emitGpuOperationFailureCheck`（GPU 操作失败分支→报告并 abort）、`generateLaunch`（生成完整的 launch 代码：按 CUDA/ROCm/CPU Simulator 三路分支，每个分支组装不同参数数组并调用对应 rt 函数，CPU 模拟器以循环模拟线程并发）。

对 C++ 读者：这个文件是 Luna 后端对 GPU 异构支持的核心。它把同一个宿主内核函数 `clone` 到不同目标，：NVPTX 使用 `llvm.nvvm.read.ptx.sreg.*` 读取 CUDA 线程索引，AMDGPU 使用 `llvm.amdgcn.workgroup_id_x`/`workitem_id_x`，CPU 模拟器则用 for 循环每次赋一个线程索引。`emitKernelHSACO` 还涉及 LLVM 的 `CloneFunctionInto`、地址空间改写、`ld.lld` 的外部链接以及 `__CLANG_OFFLOAD_BUNDLE__` 格式打包。

## 关键结构体·类·枚举

匿名命名空间内：
- `void appendLittleEndianU64(string& output, uint64_t value)`——小端写 64 位整数到 string。
- `void optimizeDeviceModule(llvm::Module& module)`——对设备模块独立跑 O3 优化管线（必须与宿主模块优化分开，否则 AMDGPU/NVPTX 会收到宿主风格的 entry allocas 和地址空间 cast）。
- `std::string makeHipModuleBundle(const string& hsaco, const string& architecture)`——把 HSACO 封装进 HIP 兼容的 Clang offload bundle（`__CLANG_OFFLOAD_BUNDLE__` 魔数 + 4K 对齐）。
- `bool dumpHsacoIfRequested(const string& hsaco, const string& symbol, const string& architecture, string& error)`——若环境变量 `LUNA_GPU_DUMP_HSACO` 被设置，把 HSACO 写入指定目录下的 `luna-<arch>-<symbol>.hsaco`。
- `void lowerDirectDeviceMemoryToGlobal(llvm::Function& function)`——对 AMDGPU 专用：把通用地址空间中的 GEP 用户（Load/Store）重定向到全局地址空间（1），消除 addrspacecast 链，使后端生成正确的全局内存指令。

## 关键函数·方法

**`bool CodeGenerator::emitKernelPTX(FunctionDecl* kernel)`**
- 根据 `mGpuTargets.cudaArchitecture`（默认 sm_52）创建 NVPTX TargetMachine。
- 创建独立 deviceModule，`CloneFunctionInto` 宿主函数，设置 `PTX_Kernel` 调用约定。
- 查找函数入口中的 index 参数 store，替换为 `llvm.nvvm.read.ptx.sreg.ctaid.x * ntid.x + tid.x`。
- 添加 `nvvm.annotations` 元数据标记为 kernel。
- `optimizeDeviceModule` 后，用 `legacy::PassManager.addPassesToEmitFile(AssemblyFile)` 生成 PTX 文本，存入 `mKernelPTX[symbol]`。
 谁调用：`CodeGeneratorModule.cpp` 的 `generate()` 在 PTX 波段。

**`bool CodeGenerator::emitKernelHSACO(FunctionDecl* kernel)`**
- 按 `mGpuTargets.rocmArchitecture`（默认 gfx1101）创建 AMDGPU TargetMachine。
- 创建 deviceModule，参数指针转 addrspace 1（全局），`AMDGPU_KERNEL` 调用约定。
- 添加 address-space cast 桥接，`CloneFunctionInto`，再设 `amdgpu-no-hostcall-ptr` 等设备专属属性。
- 私有 alloca 从 addrspace 0 转为 addrspace 5（private）。
- 替换 index 参数为 `amdgcn_workgroup_id_x * 256 + workitem_id_x`（使用 LLVM `Intrinsic::amdgcn_workgroup_id_x`/`workitem_id_x` 保证正确属性）。
- `optimizeDeviceModule` → `lowerDirectDeviceMemoryToGlobal` → 再次 `optimizeDeviceModule`。
- 用 `legacy::PassManager` 生成 ET_REL AMDGPU 对象文件，再通过 `ld.lld` 链接为 ET_DYN HSACO。
- 若 `LUNA_GPU_DUMP_HSACO` 环境变量设置，调试写入文件。
- 最终 `makeHipModuleBundle` 包装，存入 `mKernelHSACO[symbol]`。
 谁调用：`CodeGeneratorModule.cpp` 的 `generate()` 在 HSACO 波段。

**`llvm::Value* CodeGenerator::generateDeviceBufferPointer(Expr* expr)`** / **`generateHostRawPointer`**
- 递归解包 Move/Borrow/AddrOf，从 Identifier 加载值。在宿主函数中，对 `Ref<DeviceBuffer>` 额外 deref 一次得到实际设备指针。
- 谁调用：`generateLaunch`（模拟器路径）与 `generateCall`（GPU 内建）。

**`void emitGpuOperationFailureCheck(Value* operationSucceeded, Function* func)`**
- 非零检查→失败分支调 `rt_gpu_report_operation_error_and_abort` + `Unreachable`。
- 谁调用：`generateControlFlowBody`（AwaitStmt）与 `generateCall`（GPU 拷贝内建）。

**`llvm::Value* CodeGenerator::generateLaunch(LaunchExpr* launch)`**
- 解析 kernel 声明，求值线程数，构造 counter alloca。
- 创建参数数组：首元素为 counter，其余为 launch 参数（scalar 值另存 allo 供 CUDA 参数地址数组要求）。
- 按 `rt_gpu_backend_is_cuda` / `is_rocm` 条件分支：
  - CUDA 分支：调 `rt_gpu_launch_ptx(ptx, kernelName, threads, params)` 返回 event。
  - ROCm 分支：调 `rt_gpu_launch_hsaco(hsaco, hsacoSize, kernelName, threads, params)` 返回 event。
  - Simulator 分支：循环 `for idx in 0..threads`，逐次调用 kernel 函数（参数中 index 用 `generateDeviceBufferPointer` 取设备指针，scalar 直接传值）。
- 最终三路 PHI 合并 event 值返回。
- 谁调用：`generateExpr` 在检测到 `LaunchExpr` 时。谁被调：`generateDeviceBufferPointer`/`generateHostRawPointer`/`resolveFunction`/`resolveDeclaration`。

## 与周边文件·阶段的关系

- 属**代码生成阶段**的 GPU 专有子模块。
- 上游：`CodeGeneratorModule.cpp`（`generate`→两遍函数体生成后调用 emitKernelPTX/HSACO）、`CodeGeneratorExpressions.cpp`（`generateExpr`→`generateLaunch`）。
- 下游：运行时 `rt_gpu_*` 符号（在 `CodeGeneratorExecution.cpp` 的 jitRun 中绑定，也在 `generateLaunch` 中直接使用）。
- 依赖 llvm NVPTX/AMDGPU 后端（`IntrinsicsAMDGPU.h`、`TargetRegistry`、`TargetMachine`、`legacy::PassManager`）。

## 延伸阅读

1. `CodeGeneratorModule.cpp`——generate 中 GPU 码物的两遍调度。
2. `CodeGeneratorExecution.cpp`——jitRun 中 rt_gpu_* 符号绑定。
3. LLVM 文档：`Intrinsic::amdgcn_workgroup_id_x`、NVPTX 的 `llvm.nvvm.read.ptx.sreg.*` 系列。
4. `../runtime/Runtime.h`——GPU 运行期 API。

---

---
title: src/codegen/CodeGeneratorIterator.cpp
path: src/codegen/CodeGeneratorIterator.cpp
阶段: 代码生成 (CodeGen)——迭代器管线 LLVM 生成
语言: C++
---
