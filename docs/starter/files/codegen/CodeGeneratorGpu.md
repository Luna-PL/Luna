# src/codegen/CodeGeneratorGpu.cpp — GPU Kernel Code Generation (PTX/HSACO), Device Pointer Resolution, and Launch Emission

## What This File Does

This file implements all GPU-related LLVM code generation: `emitKernelPTX` (clones the host kernel function into an NVPTX module, replaces the index with `blockIdx*blockDim+threadIdx`, and compiles it to PTX text), `emitKernelHSACO` (clones it into an AMDGPU module, performs address-space conversion and private-alloca rewriting, compiles it to an HSACO file and links it via lld into a dynamic HSA code object, finally wrapping it in the HIP Module Bundle format), `generateDeviceBufferPointer`/`generateHostRawPointer` (resolves device/host memory pointer paths), `emitGpuOperationFailureCheck` (GPU operation failure branch → report and abort), and `generateLaunch` (generates the complete launch code: a three-way branch for CUDA/ROCm/CPU Simulator, where each branch assembles a different parameter array and calls the corresponding rt function; the CPU simulator emulates thread concurrency with a loop).

For C++ readers: this file is the core of the Luna backend's heterogeneous GPU support. It clones the same host kernel function to different targets: NVPTX reads CUDA thread indices via `llvm.nvvm.read.ptx.sreg.*`, AMDGPU uses `llvm.amdgcn.workgroup_id_x`/`workitem_id_x`, and the CPU simulator assigns one thread index per iteration of a for loop. `emitKernelHSACO` also involves LLVM's `CloneFunctionInto`, address-space rewriting, external linking with `ld.lld`, and packaging into the `__CLANG_OFFLOAD_BUNDLE__` format.

## Key Structs, Classes, and Enums

Within the anonymous namespace:

- `void appendLittleEndianU64(string& output, uint64_t value)` — appends a 64-bit integer to a string in little-endian byte order.
- `void optimizeDeviceModule(llvm::Module& module)` — runs an independent O3 optimization pipeline on the device module (it must be kept separate from host-module optimization; otherwise AMDGPU/NVPTX would receive host-style entry allocas and address-space casts).
- `std::string makeHipModuleBundle(const string& hsaco, const string& architecture)` — wraps HSACO into a HIP-compatible Clang offload bundle (`__CLANG_OFFLOAD_BUNDLE__` magic number + 4K alignment).
- `bool dumpHsacoIfRequested(const string& hsaco, const string& symbol, const string& architecture, string& error)` — if the environment variable `LUNA_GPU_DUMP_HSACO` is set, writes the HSACO to `luna-<arch>-<symbol>.hsaco` in the specified directory.
- `void lowerDirectDeviceMemoryToGlobal(llvm::Function& function)` — AMDGPU-specific: redirects GEP users (Load/Store) in the generic address space to the global address space (1), eliminating addrspacecast chains so the backend emits correct global memory instructions.

## Key Functions and Methods

**`bool CodeGenerator::emitKernelPTX(FunctionDecl* kernel)`**
- Creates an NVPTX TargetMachine based on `mGpuTargets.cudaArchitecture` (default sm_52).
- Creates a separate deviceModule, clones the host function with `CloneFunctionInto`, and sets the `PTX_Kernel` calling convention.
- Finds the store of the index parameter in the function entry and replaces it with `llvm.nvvm.read.ptx.sreg.ctaid.x * ntid.x + tid.x`.
- Adds `nvvm.annotations` metadata to mark it as a kernel.
- After `optimizeDeviceModule`, emits PTX text via `legacy::PassManager.addPassesToEmitFile(AssemblyFile)` and stores it in `mKernelPTX[symbol]`.
 Called by: `generate()` in `CodeGeneratorModule.cpp` during the PTX pass.

**`bool CodeGenerator::emitKernelHSACO(FunctionDecl* kernel)`**
- Creates an AMDGPU TargetMachine based on `mGpuTargets.rocmArchitecture` (default gfx1101).
- Creates the deviceModule, converts parameter pointers to addrspace 1 (global), with the `AMDGPU_KERNEL` calling convention.
- Adds address-space cast bridges, clones with `CloneFunctionInto`, then sets device-specific attributes such as `amdgpu-no-hostcall-ptr`.
- Converts private allocas from addrspace 0 to addrspace 5 (private).
- Replaces the index parameter with `amdgcn_workgroup_id_x * 256 + workitem_id_x` (using LLVM's `Intrinsic::amdgcn_workgroup_id_x`/`workitem_id_x` to guarantee correct attributes).
- `optimizeDeviceModule` → `lowerDirectDeviceMemoryToGlobal` → `optimizeDeviceModule` again.
- Uses `legacy::PassManager` to generate an ET_REL AMDGPU object file, then links it into an ET_DYN HSACO via `ld.lld`.
- If the `LUNA_GPU_DUMP_HSACO` environment variable is set, writes the output to a file for debugging.
- Finally wraps it with `makeHipModuleBundle` and stores it in `mKernelHSACO[symbol]`.
 Called by: `generate()` in `CodeGeneratorModule.cpp` during the HSACO pass.

**`llvm::Value* CodeGenerator::generateDeviceBufferPointer(Expr* expr)`** / **`generateHostRawPointer`**
- Recursively unwraps Move/Borrow/AddrOf and loads the value from the Identifier. Inside host functions, dereferences `Ref<DeviceBuffer>` one extra time to obtain the actual device pointer.
- Called by: `generateLaunch` (simulator path) and `generateCall` (GPU intrinsics).

**`void emitGpuOperationFailureCheck(Value* operationSucceeded, Function* func)`**
- Non-zero check → the failure branch calls `rt_gpu_report_operation_error_and_abort` followed by `Unreachable`.
- Called by: `generateControlFlowBody` (AwaitStmt) and `generateCall` (GPU copy intrinsics).

**`llvm::Value* CodeGenerator::generateLaunch(LaunchExpr* launch)`**
- Resolves the kernel declaration, evaluates the thread count, and constructs the counter alloca.
- Creates the parameter array: the first element is the counter, the rest are the launch parameters (scalar values are additionally stored into an alloca to satisfy CUDA's parameter address-array requirement).
- Branches on `rt_gpu_backend_is_cuda` / `is_rocm`:
  - CUDA branch: calls `rt_gpu_launch_ptx(ptx, kernelName, threads, params)` and returns an event.
  - ROCm branch: calls `rt_gpu_launch_hsaco(hsaco, hsacoSize, kernelName, threads, params)` and returns an event.
  - Simulator branch: loops `for idx in 0..threads`, invoking the kernel function each iteration (the index argument resolves the device pointer via `generateDeviceBufferPointer`, and scalars are passed by value).
- Finally merges the three branches with a PHI node and returns the event value.
- Called by: `generateExpr` when it detects a `LaunchExpr`. Calls: `generateDeviceBufferPointer`/`generateHostRawPointer`/`resolveFunction`/`resolveDeclaration`.

## Relationship to Surrounding Files and Pipeline Stages

- A GPU-specific submodule of the **code generation stage**.
- Upstream: `CodeGeneratorModule.cpp` (`generate` calls emitKernelPTX/HSACO after the two-pass function-body generation) and `CodeGeneratorExpressions.cpp` (`generateExpr` → `generateLaunch`).
- Downstream: runtime `rt_gpu_*` symbols (bound in `jitRun` in `CodeGeneratorExecution.cpp` and also used directly in `generateLaunch`).
- Depends on the LLVM NVPTX/AMDGPU backends (`IntrinsicsAMDGPU.h`, `TargetRegistry`, `TargetMachine`, `legacy::PassManager`).

## Further Reading

1. `CodeGeneratorModule.cpp` — the two-pass dispatch of GPU code artifacts in `generate`.
2. `CodeGeneratorExecution.cpp` — binding of the `rt_gpu_*` symbols in `jitRun`.
3. LLVM documentation: `Intrinsic::amdgcn_workgroup_id_x`, the NVPTX `llvm.nvvm.read.ptx.sreg.*` family.
4. `../runtime/Runtime.h` — the GPU runtime API.

---
title: src/codegen/CodeGeneratorIterator.cpp
path: src/codegen/CodeGeneratorIterator.cpp
stage: Code Generation (CodeGen) — iterator pipeline LLVM generation
language: C++
---
