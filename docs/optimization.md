# 宿主优化级别

## English summary

Luna exposes `-O0`, `-O2`, and `-O3` for both JIT and AOT. `-O0` is the default for
experimental control-flow work; `-O2` and `-O3` enable the standard LLVM host
pipeline. Device kernels use a separate target-specific LLVM pipeline. Performance
numbers should distinguish compiler time, process startup, synchronization, and
kernel execution.

Luna 的 Alpha 默认使用 `-O0`，以便调试与实验性 interceptor / context / slot 行为保持最直接的 IR 对应关系。`run` 和 `build` 都支持以下选择：

```sh
./build/luna run examples/operators.luna -O2
./build/luna build examples/operators.luna -O3
./build/luna build examples/operators.luna --opt=O2
```

- `-O0`：不运行可选 LLVM 优化管线。
- `-O2`：运行 LLVM 的标准 per-module 速度优化管线，包括 SSA 栈提升、标量替换、常量传播、死代码删除与成本受控的内联。
- `-O3`：运行更积极的 LLVM 速度优化管线；编译时间和代码大小可能增加。

优化前后都会验证宿主 LLVM module。若优化暴露无效 IR，编译会以 `CGN0001` 失败，而不会继续交给 JIT 或系统链接器。AOT 构建还会把同一个 `-O0/-O2/-O3` 级别传给最终的 clang++ native backend；因此 `-O3` 不会停留在文本 LLVM IR 阶段。

设备代码不再绕过 LLVM 优化：每个 CUDA/ROCm kernel 克隆体会运行独立的 LLVM O3 module pipeline，随后交给对应 target machine；ROCm 还会在 global address-space ABI 修正后再做一次收尾优化。AMDGPU/NVPTX target machine 使用 aggressive codegen level，以匹配 HIP/Clang 的发布构建路径。

ROCm 内核参数在 HSA 入口使用 global address space（addrspace(1)），最终 ISA 必须使用 `global_load/global_store`；这条约束由 `luna.rocm-isa-abi` 离线测试保护。它不需要实际 GPU，只需要 LLVM 的 `llvm-objdump`。

设备入口还显式声明 `amdgpu-no-hostcall-ptr`、`amdgpu-no-queue-ptr`、`amdgpu-no-heap-ptr` 等属性。Luna kernel 子语言禁止这些能力，因此无需为它们保留隐式 kernarg。当前 gfx1101 descriptor 的 kernarg 从 272 字节降为 16 字节，SGPR 从 11 降为 9。
