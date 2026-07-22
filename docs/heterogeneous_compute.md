# Heterogeneous compute (initial ABI)

## 中文说明

异构计算目前提供 CPU simulator、CUDA 和 ROCm 三条路径。`device_buffer`、`launch`
和 `await` 具有独立的所有权与事件生命周期；默认 simulator 方便无 GPU 环境回归。
设备端函数不能使用 host continuation 或 host-only 动态 effect，硬件后端不可用时
必须显式报错，不会静默切换后端。

Luna now has a safe, explicit asynchronous compute surface. The default backend is a CPU simulator so programs run everywhere. CUDA and ROCm backends clone each `kernel fn` into an LLVM device module, emit PTX or a linked HSA code object (HSACO), load it through the CUDA Driver API or HIP Module API, dispatch it, and return a real device event. The ROCm path packages the linked HSACO in the Clang HIP module bundle that `hipModuleLoadData` accepts. The source ABI keeps allocation, scalar transfers, dispatch, and synchronization separate from ordinary host memory.

```luna
kernel fn add_one(index: i32, data: &mut device_buffer<i32>) {
    let value = gpu_load_i32(data, index);
    gpu_store_i32(data, index, value + 1);
}

fn main() -> i32 {
    linear let data = gpu_alloc_i32(8);
    gpu_store_i32(borrow mut data, 0, 41);

    let done = launch add_one[threads: 8](borrow mut data);
    await done;

    print(gpu_load_i32(borrow data, 0)); // 42
    gpu_free(move data);
    return 0;
}
```

`kernel fn` has a fixed initial ABI:

- Its first parameter is explicitly `index: i32`; `launch` supplies one logical index per thread.
- It returns `unit`, is neither generic nor `extern`/`constexpr`, and may only call `gpu_load_i32` and `gpu_store_i32` in this initial ABI.
- Scalars are passed by value. A buffer is passed only as `&device_buffer<T>` or `&mut device_buffer<T>`.
- Kernel bodies form a `DeviceMemory`-only sublanguage: scalar bindings, arithmetic, branches, loops, and the two device built-ins are allowed. `slot`, `apply`, `resume()`, `abort()`, `await`, `launch`, `new`, `free`, FFI, closures, reflection, and ordinary host calls are rejected. This prevents host continuations or resource effects from entering SIMT code.

`device_buffer<T>` and the `event` returned from `launch` are automatically linear. Buffer arguments must be explicit named borrows at the launch site. Launching a buffer produces an in-flight loan that lasts until the corresponding `await`:

- The buffer cannot be borrowed, accessed, moved, or freed while in flight.
- An event cannot be ignored, copied, returned, or allowed to leave its scope unawaited.
- An event may be transferred with `move`; the in-flight loan transfers with it, and the receiving binding must be awaited.
- `gpu_free(move buffer)` is the only release operation for a device buffer.

## Failure behavior

A CUDA or ROCm launch that cannot load a module, resolve a kernel, create an
event, or submit work returns an internal invalid event handle. It is not
treated as a completed dispatch. `await event` validates and synchronizes that
handle; on launch or synchronization failure it prints the selected backend
and the vendor/runtime error, then terminates with a non-zero status. This is
also true for AOT binaries. The simulator's completed event succeeds through
the same `await` ABI.

## Backend selection

The simulator is the default and runs dispatch synchronously under the hood while preserving the asynchronous ownership contract:

PTX/HSACO selection happens while compiling and only inspects
`LUNA_GPU_BACKEND` or the explicit offline-emission flags. It does not initialize
CUDA/HIP or require a physical GPU on the build machine. The generated JIT/AOT
entry point initializes and validates the selected runtime only when reachable
kernel capability is actually present.

```sh
./build/luna run examples/heterogeneous.luna
# or explicitly:
LUNA_GPU_BACKEND=sim ./build/luna run examples/heterogeneous.luna
```

Select CUDA with a functioning NVIDIA driver; no CUDA Toolkit or NVRTC installation is needed at build time because the Driver API is loaded dynamically:

```sh
LUNA_GPU_BACKEND=cuda ./build/luna run examples/heterogeneous.luna
```

CUDA currently emits one-dimensional grids with 256 threads per block and uses the default CUDA stream. The source `index` parameter is lowered to `blockIdx.x * blockDim.x + threadIdx.x` in PTX, while the simulator provides the same value through its dispatch loop. `await event` synchronizes and destroys the associated CUDA event. If CUDA is requested but the Driver API cannot initialize, Luna fails before compilation with the driver error; it never silently falls back to the simulator.

Select ROCm for an AMD GPU. Luna loads `libamdhip64.so` dynamically, so its headers and compiler are not build dependencies, but a working ROCm HIP runtime and kernel driver are required to execute on the device:

```sh
LUNA_GPU_BACKEND=rocm ./build/luna run examples/heterogeneous.luna
```

ROCm uses the same one-dimensional, 256-thread launch geometry. The source `index` becomes `workgroup_id_x * 256 + workitem_id_x`; `await event` synchronizes and destroys the HIP event. Luna defaults its offline AMDGPU code generation to `gfx1101`, suitable for Navi 32 / RX 7700 XT and RX 7800 XT. Set `LUNA_AMDGPU_ARCH` for another GPU, for example `LUNA_AMDGPU_ARCH=gfx1030`. AMDGPU emission also requires the matching LLVM linker, `ld.lld`, in `PATH`; it is normally installed with LLVM and is only invoked when emitting a ROCm/offline AMDGPU kernel.

On an ROCm-equipped AMD machine, enable the opt-in hardware smoke test to run
the basic kernel through both JIT and AOT. The test dispatches eight threads
and confirms the host reads back `42`; it is a correctness smoke test, not a
thermal or throughput workload:

```sh
cmake -S . -B build -DLUNA_ENABLE_ROCM_SMOKE=ON
cmake --build build
ctest --test-dir build -L rocm --output-on-failure
```

It is disabled by default so ordinary CPU-only CI stays deterministic. Set
`LUNA_AMDGPU_ARCH` in the CTest environment when the GPU is not `gfx1101`.

Build machines without a GPU can validate the NVPTX lowering while still executing with the simulator:

```sh
LUNA_GPU_EMIT_PTX=1 ./build/luna run examples/heterogeneous.luna
```

This emits PTX during compilation and takes the simulator branch at runtime.
For an AOT executable that may later run with CUDA, use the same variable while building so PTX is embedded in the emitted host IR:

```sh
LUNA_GPU_EMIT_PTX=1 ./build/luna build examples/heterogeneous.luna
LUNA_GPU_BACKEND=cuda ./examples/heterogeneous
```

The analogous ROCm validation path emits a real HSACO while execution still stays on the simulator. It is useful before ROCm is installed on a build machine:

```sh
LUNA_GPU_EMIT_AMDGPU=1 ./build/luna run examples/heterogeneous.luna
```

For an AOT executable that will run on an AMD GPU later, embed the HSACO at build time and select ROCm when launching it:

```sh
LUNA_GPU_EMIT_AMDGPU=1 ./build/luna build examples/heterogeneous.luna
LUNA_GPU_BACKEND=rocm ./examples/heterogeneous
```

Kernel declarations may carry ordinary Metadata, for example `@version(1, 0, 0) kernel fn bump(...)`. A named `launch bump[threads: n](...)` currently requires that `bump` resolve to one kernel declaration; an ambiguous metadata family is rejected. Runtime kernel-family binding will be an explicit dynamic operation rather than postfix version syntax.

Kernels are code-generated only when referenced by a reachable lowered launch. Merely declaring a kernel emits a deferred MoonIR recipe and no `rt_gpu_*` host symbol. `--reserve-kernel-runtime` explicitly retains kernel code and runtime initialization when a host needs future dynamic kernel capability.

Initial i32 device operations are:

- `gpu_alloc_i32(count) -> device_buffer<i32>`
- `gpu_load_i32(borrow buffer, index) -> i32`
- `gpu_store_i32(borrow mut buffer, index, value)`
- `gpu_free(move buffer)`

Bulk host/device transfer is available through an explicit low-level ABI:

- `gpu_copy_from_host_i32(borrow mut buffer, borrow host, count)`
- `gpu_copy_to_host_i32(borrow mut host, borrow buffer, count)`

`host` has type `raw<i32>` and is commonly returned by an FFI allocator. The
upload requires a mutable device borrow; the download requires a mutable host
borrow. This preserves the borrow checker at both endpoints while the language
does not yet provide a safe host array or slice type. A negative count is
rejected for literals and fails at runtime for dynamic values.

The valid and invalid examples are in `examples/heterogeneous*.luna`.
Benchmark methodology and the JIT/AOT sampling script are in
[heterogeneous_benchmarks.md](heterogeneous_benchmarks.md).
